use std::borrow::Cow;
use std::future::Future;
use std::net::SocketAddr;
use std::sync::Arc;

use axum::{
    body::Body,
    http::{header, StatusCode, Uri},
    middleware,
    response::Response,
    routing::{get, post},
    Router,
};
use rust_embed::Embed;
use tokio::net::{TcpListener, TcpStream};
use tokio_rustls::server::TlsStream;
use tokio_rustls::TlsAcceptor;
use tracing::{info, warn};
use tracing_subscriber::EnvFilter;

use esp_smart_car_backend::{api, serial, websocket, AppState};

/// 静态文件嵌入：前端资源编译进二进制
#[derive(Embed)]
#[folder = "frontend/dist/"]
struct Assets;

/// 主函数
#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // 尝试加载 .env 文件（仅在开发目录中有效；exe 移动到其他位置运行时使用上述默认值）
    // dotenvy::dotenv() 从当前工作目录向上查找 .env，不存在时静默返回 Err —— 这不影响正常启动
    let _ = dotenvy::dotenv();

    // 初始化日志：优先使用 RUST_LOG 环境变量（.env 或系统环境），未设置时回退到 "info"
    // 不使用 std::env::set_var 修改全局环境（SubTask 1.19）
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .init();

    info!("智能车桌面端后端启动");
    info!("版本: 1.2.0");

    // 创建应用状态
    let state = Arc::new(AppState::new());

    // 启动串口通信任务（出错时指数退避重启；正常退出则停止，不再重启）
    let serial_state = state.clone();
    tokio::spawn(async move {
        let mut consecutive_failures: u32 = 0;
        loop {
            if let Err(e) = serial::run_serial_task(serial_state.clone()).await {
                consecutive_failures += 1;
                // 指数退避：3s, 6s, 12s, 24s, 最大 60s；限制移位量避免 65 次后 panic
                let shift = std::cmp::min(consecutive_failures.saturating_sub(1), 4);
                let delay_secs = std::cmp::min(3u64 * (1u64 << shift), 60);
                warn!(
                    "串口任务错误(连续第{}次): {}, {}秒后重启",
                    consecutive_failures, e, delay_secs
                );
                tokio::time::sleep(tokio::time::Duration::from_secs(delay_secs)).await;
            } else {
                // 正常退出（如主动断开），业务上应停止而非无限重启（SubTask 1.20）
                info!("串口任务正常退出，不再重启");
                break;
            }
        }
    });

    // 启动串口扫描任务
    let port_scan_state = state.clone();
    tokio::spawn(async move {
        serial::run_port_scan_task(port_scan_state).await;
    });

    // 构建路由
    // API 路由（不 fallback，确保 API 404 返回 JSON 而非 HTML）
    let api_routes = Router::new()
        .route("/api/command", post(api::handle_command))
        .route("/api/status", get(api::get_status))
        .route("/api/connect", post(api::connect_serial))
        .route("/api/disconnect", post(api::disconnect_serial))
        .route("/api/ports", get(api::list_ports))
        .route("/api/ble-devices", get(api::get_ble_devices))
        .route_layer(middleware::from_fn_with_state(
            state.clone(),
            api::auth_middleware,
        ));

    info!("前端资源已嵌入二进制");

    let app = Router::new()
        .route("/ws", get(websocket::ws_handler))
        .merge(api_routes)
        .fallback(get(static_handler))
        .with_state(state.clone());

    // 监听地址（仅本地访问，不暴露给其他设备）
    let addr = SocketAddr::from(([127, 0, 0, 1], 8080));

    // 若配置了 TLS_CERT/TLS_KEY，则启用 HTTPS/WSS
    match load_tls_config().await? {
        Some(tls_acceptor) => {
            info!("Web服务器监听 (TLS): https://{}", addr);
            info!("WebSocket端点: wss://{}/ws", addr);
            let listener = TcpListener::bind(addr).await?;
            let tls_listener = TlsListener {
                inner: listener,
                acceptor: tls_acceptor,
            };
            axum::serve(tls_listener, app).await?;
        }
        None => {
            info!("Web服务器监听: http://{}", addr);
            info!("WebSocket端点: ws://{}/ws", addr);
            let listener = TcpListener::bind(addr).await?;
            axum::serve(listener, app).await?;
        }
    }

    Ok(())
}

/// 从环境变量 TLS_CERT/TLS_KEY 加载 TLS 配置
///
/// 单侧配置（只有 TLS_CERT 或只有 TLS_KEY）时返回 Err 明确告知，
/// 避免用户误以为 TLS 已启用而实际走明文（SubTask 1.16）
async fn load_tls_config() -> anyhow::Result<Option<TlsAcceptor>> {
    let cert_path = std::env::var("TLS_CERT").ok().filter(|p| !p.is_empty());
    let key_path = std::env::var("TLS_KEY").ok().filter(|p| !p.is_empty());

    match (cert_path, key_path) {
        (None, None) => Ok(None),
        (Some(cert_path), Some(key_path)) => {
            let certs: Vec<rustls::pki_types::CertificateDer<'static>> =
                rustls_pemfile::certs(&mut std::io::BufReader::new(std::fs::File::open(
                    &cert_path,
                )?))
                .collect::<Result<Vec<_>, _>>()?;
            let key = rustls_pemfile::private_key(&mut std::io::BufReader::new(
                std::fs::File::open(&key_path)?,
            ))?
            .ok_or_else(|| anyhow::anyhow!("无法解析 TLS 私钥"))?;

            let config = rustls::ServerConfig::builder()
                .with_no_client_auth()
                .with_single_cert(certs, key)?;

            info!("TLS 配置已加载: cert={}, key={}", cert_path, key_path);
            Ok(Some(TlsAcceptor::from(Arc::new(config))))
        }
        (Some(cert), None) => Err(anyhow::anyhow!(
            "已设置 TLS_CERT={} 但未设置 TLS_KEY，无法启用 TLS。请同时配置 TLS_CERT 和 TLS_KEY，或同时清除两者",
            cert
        )),
        (None, Some(key)) => Err(anyhow::anyhow!(
            "已设置 TLS_KEY={} 但未设置 TLS_CERT，无法启用 TLS。请同时配置 TLS_CERT 和 TLS_KEY，或同时清除两者",
            key
        )),
    }
}

/// TLS 监听器包装器，使 axum::serve 能够接受 TLS 连接
struct TlsListener {
    inner: TcpListener,
    acceptor: TlsAcceptor,
}

impl axum::serve::Listener for TlsListener {
    type Io = TlsStream<TcpStream>;
    type Addr = SocketAddr;

    fn accept(&mut self) -> impl Future<Output = (Self::Io, Self::Addr)> + Send {
        let acceptor = self.acceptor.clone();
        async move {
            loop {
                match self.inner.accept().await {
                    Ok((stream, addr)) => match acceptor.accept(stream).await {
                        Ok(stream) => return (stream, addr),
                        Err(e) => warn!("TLS 握手失败: {}", e),
                    },
                    Err(e) => warn!("TLS accept 失败: {}", e),
                }
            }
        }
    }

    fn local_addr(&self) -> Result<SocketAddr, std::io::Error> {
        self.inner.local_addr()
    }
}

/// 静态文件处理器（嵌入二进制）
/// 先尝试匹配请求路径的文件，找不到则返回 index.html（SPA fallback）
async fn static_handler(uri: Uri) -> Response<Body> {
    fn build_response(
        status: StatusCode,
        body: Body,
        content_type: Option<&str>,
    ) -> Response<Body> {
        let mut builder = Response::builder().status(status);
        if let Some(ct) = content_type {
            builder = builder.header(header::CONTENT_TYPE, ct);
        }
        // axum Body 构建不会失败（仅设置状态码和头部），单层 expect 即可（SubTask 1.18）
        builder.body(body).expect("axum Body 构建不会失败")
    }

    // rust-embed 返回 Cow<'static, [u8]>；按借用/ owned 分流，避免对静态资源额外拷贝（SubTask 1.17）
    fn body_from_cow(data: Cow<'static, [u8]>) -> Body {
        match data {
            Cow::Borrowed(b) => Body::from(b),
            Cow::Owned(v) => Body::from(v),
        }
    }

    let path = uri.path().trim_start_matches('/');
    let path = if path.is_empty() { "index.html" } else { path };

    match Assets::get(path) {
        Some(file) => {
            let mime = mime_guess::from_path(path).first_or_octet_stream();
            build_response(
                StatusCode::OK,
                body_from_cow(file.data),
                Some(mime.as_ref()),
            )
        }
        None => match Assets::get("index.html") {
            Some(index) => build_response(
                StatusCode::OK,
                body_from_cow(index.data),
                Some("text/html; charset=utf-8"),
            ),
            None => build_response(StatusCode::NOT_FOUND, Body::from("404 Not Found"), None),
        },
    }
}

#[cfg(test)]
mod tests {
    use esp_smart_car_backend::{AppState, MutexExt};

    /// 测试 AppState 初始状态（使用 new_test 避免依赖环境变量，SubTask 1.21）
    #[test]
    fn test_app_state_new() {
        let state = AppState::new_test();
        let current_speed = state
            .current_speed
            .load(std::sync::atomic::Ordering::Relaxed);
        assert_eq!(current_speed, 5);
        let video_frame = state.video_frame.lock_or_recover("video_frame");
        assert!(video_frame.is_none());
    }
}
