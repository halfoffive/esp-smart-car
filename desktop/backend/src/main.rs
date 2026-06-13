use std::net::SocketAddr;
use std::sync::Arc;

use axum::{
    body::Body,
    http::{header, StatusCode, Uri},
    response::Response,
    routing::{get, post},
    Router,
};
use rust_embed::Embed;
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
    // 设置默认日志级别（系统环境或 .env 都未设置时，默认使用 info）
    if std::env::var("RUST_LOG").is_err() {
        std::env::set_var("RUST_LOG", "info");
    }

    // 尝试加载 .env 文件（仅在开发目录中有效；exe 移动到其他位置运行时使用上述默认值）
    // dotenvy::dotenv() 从当前工作目录向上查找 .env，不存在时静默返回 Err —— 这不影响正常启动
    let _ = dotenvy::dotenv();

    // 初始化日志
    tracing_subscriber::fmt()
        .with_env_filter(EnvFilter::from_default_env())
        .init();

    info!("智能车桌面端后端启动");
    info!("版本: 1.2.0");

    // 创建应用状态
    let state = Arc::new(AppState::new());

    // 启动串口通信任务（退出后自动重启，指数退避防止"假死"时频繁重试）
    let serial_state = state.clone();
    tokio::spawn(async move {
        let mut consecutive_failures: u32 = 0;
        loop {
            if let Err(e) = serial::run_serial_task(serial_state.clone()).await {
                consecutive_failures += 1;
                // 指数退避：3s, 6s, 12s, 24s, 最大 60s
                let delay_secs = std::cmp::min(
                    3u64 * (1u64 << consecutive_failures.saturating_sub(1)),
                    60,
                );
                warn!(
                    "串口任务错误(连续第{}次): {}, {}秒后重启",
                    consecutive_failures, e, delay_secs
                );
                tokio::time::sleep(tokio::time::Duration::from_secs(delay_secs)).await;
            } else {
                // 正常退出（如断开连接），重置退避计数，短暂等待后重启
                consecutive_failures = 0;
                tokio::time::sleep(tokio::time::Duration::from_secs(1)).await;
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
        .route("/api/ble-devices", get(api::get_ble_devices));

    info!("前端资源已嵌入二进制");

    let app = Router::new()
        .route("/ws", get(websocket::ws_handler))
        .merge(api_routes)
        .fallback(get(static_handler))
        .with_state(state);

    // 监听地址（仅本地访问，不暴露给其他设备）
    let addr = SocketAddr::from(([127, 0, 0, 1], 8080));
    info!("Web服务器监听: http://{}", addr);
    info!("WebSocket端点: ws://{}/ws", addr);

    // 启动服务器
    let listener = tokio::net::TcpListener::bind(addr).await?;
    axum::serve(listener, app).await?;

    Ok(())
}

/// 静态文件处理器（嵌入二进制）
/// 先尝试匹配请求路径的文件，找不到则返回 index.html（SPA fallback）
async fn static_handler(uri: Uri) -> Response<Body> {
    let path = uri.path().trim_start_matches('/');
    let path = if path.is_empty() { "index.html" } else { path };

    match Assets::get(path) {
        Some(file) => {
            let mime = mime_guess::from_path(path).first_or_octet_stream();
            Response::builder()
                .status(StatusCode::OK)
                .header(header::CONTENT_TYPE, mime.as_ref())
                .body(Body::from(file.data.into_owned()))
                .expect("响应构建不应失败")
        }
        None => match Assets::get("index.html") {
            Some(index) => Response::builder()
                .status(StatusCode::OK)
                .header(header::CONTENT_TYPE, "text/html; charset=utf-8")
                .body(Body::from(index.data.into_owned()))
                .expect("响应构建不应失败"),
            None => Response::builder()
                .status(StatusCode::NOT_FOUND)
                .body(Body::from("404 Not Found"))
                .expect("响应构建不应失败"),
        },
    }
}

#[cfg(test)]
mod tests {
    use esp_smart_car_backend::AppState;

    /// 测试 AppState 初始状态
    #[test]
    fn test_app_state_new() {
        let state = AppState::new();
        let current_speed = state
            .current_speed
            .load(std::sync::atomic::Ordering::Relaxed);
        assert_eq!(current_speed, 5);
        let video_frame = state.video_frame.lock().expect("video_frame lock poisoned");
        assert!(video_frame.is_none());
        let video_frame_b64 = state.video_frame_b64.lock().expect("video_frame_b64 lock poisoned");
        assert!(video_frame_b64.is_none());
    }
}
