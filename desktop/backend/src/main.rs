use std::borrow::Cow;
use std::future::Future;
use std::net::SocketAddr;
use std::sync::Arc;

use axum::{
    body::Body,
    http::{header, HeaderValue, Method, Request, StatusCode, Uri},
    middleware::{self, Next},
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
    match dotenvy::dotenv() {
        Ok(_) => {
            info!(".env 文件已加载");
        }
        Err(e) => {
            let err_str = e.to_string();
            if !err_str.contains("not found") && !err_str.contains("No such file") {
                warn!(".env 文件解析失败: {}", e);
            }
        }
    }

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

    let mut app = Router::new()
        .route("/ws", get(websocket::ws_handler))
        .merge(api_routes)
        .fallback(get(static_handler))
        .with_state(state.clone());

    if cfg!(debug_assertions) {
        app = app.layer(middleware::from_fn(cors_middleware));
        info!("开发模式：CORS 中间件已启用（允许 localhost 跨域）");
    }

    let host = std::env::var("HOST").unwrap_or_else(|_| "127.0.0.1".to_string());
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|p| p.parse().ok())
        .unwrap_or(8080);
    let addr: SocketAddr = format!("{}:{}", host, port)
        .parse()
        .unwrap_or_else(|_| SocketAddr::from(([127, 0, 0, 1], 8080)));

    info!("监听地址: {}", addr);

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
            let cert_path_clone = cert_path.clone();
            let key_path_clone = key_path.clone();
            let tls_result = tokio::task::spawn_blocking(move || -> anyhow::Result<_> {
                let certs: Vec<rustls::pki_types::CertificateDer<'static>> =
                    rustls_pemfile::certs(&mut std::io::BufReader::new(std::fs::File::open(
                        &cert_path_clone,
                    )?))
                    .collect::<Result<Vec<_>, _>>()?;
                let key = rustls_pemfile::private_key(&mut std::io::BufReader::new(
                    std::fs::File::open(&key_path_clone)?,
                ))?
                .ok_or_else(|| anyhow::anyhow!("无法解析 TLS 私钥"))?;
                Ok((certs, key))
            })
            .await??;

            let (certs, key) = tls_result;
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
                tokio::time::sleep(tokio::time::Duration::from_millis(50)).await;
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
        cache_control: Option<&str>,
    ) -> Response<Body> {
        let mut builder = Response::builder().status(status);
        if let Some(ct) = content_type {
            builder = builder.header(header::CONTENT_TYPE, ct);
        }
        if let Some(cc) = cache_control {
            builder = builder.header(header::CACHE_CONTROL, cc);
        }
        builder.body(body).unwrap_or_else(|_| {
            let mut fallback = Response::new(Body::from("500 Internal Server Error"));
            *fallback.status_mut() = StatusCode::INTERNAL_SERVER_ERROR;
            fallback
        })
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

    let cache_control = if path == "index.html" {
        Some("no-cache, no-store, must-revalidate")
    } else if path.starts_with("assets/") {
        Some("public, max-age=31536000, immutable")
    } else {
        None
    };

    match Assets::get(path) {
        Some(file) => {
            let mime = mime_guess::from_path(path).first_or_octet_stream();
            build_response(
                StatusCode::OK,
                body_from_cow(file.data),
                Some(mime.as_ref()),
                cache_control,
            )
        }
        None => match Assets::get("index.html") {
            Some(index) => build_response(
                StatusCode::OK,
                body_from_cow(index.data),
                Some("text/html; charset=utf-8"),
                Some("no-cache, no-store, must-revalidate"),
            ),
            None => build_response(StatusCode::NOT_FOUND, Body::from("404 Not Found"), None, None),
        },
    }
}

/// 开发环境 CORS 中间件：允许 localhost 来源的跨域请求
async fn cors_middleware(request: Request<Body>, next: Next) -> Response {
    let origin = request
        .headers()
        .get(header::ORIGIN)
        .and_then(|v| v.to_str().ok())
        .unwrap_or("")
        .to_string();

    let is_localhost = origin.starts_with("http://localhost:")
        || origin.starts_with("http://127.0.0.1:")
        || origin.starts_with("http://[::1]:")
        || origin.starts_with("https://localhost:")
        || origin.starts_with("https://127.0.0.1:");

    let is_preflight = request.method() == Method::OPTIONS;

    if is_preflight {
        let mut response = Response::new(Body::empty());
        *response.status_mut() = StatusCode::NO_CONTENT;
        if is_localhost {
            if let Ok(v) = HeaderValue::from_str(&origin) {
                response.headers_mut().insert(header::ACCESS_CONTROL_ALLOW_ORIGIN, v);
            }
            response.headers_mut().insert(
                header::ACCESS_CONTROL_ALLOW_METHODS,
                HeaderValue::from_static("GET, POST, OPTIONS"),
            );
            response.headers_mut().insert(
                header::ACCESS_CONTROL_ALLOW_HEADERS,
                HeaderValue::from_static("Content-Type, Authorization"),
            );
            response.headers_mut().insert(
                header::ACCESS_CONTROL_MAX_AGE,
                HeaderValue::from_static("86400"),
            );
        }
        return response;
    }

    let mut response = next.run(request).await;
    if is_localhost {
        if let Ok(v) = HeaderValue::from_str(&origin) {
            response.headers_mut().insert(header::ACCESS_CONTROL_ALLOW_ORIGIN, v);
        }
        response.headers_mut().insert(
            header::VARY,
            HeaderValue::from_static("Origin"),
        );
    }
    response
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
