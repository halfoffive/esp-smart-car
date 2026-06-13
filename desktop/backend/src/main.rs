use std::net::SocketAddr;
use std::sync::Arc;

use axum::{
    routing::{get, post},
    Router,
};
use tracing::{info, warn};

use esp_smart_car_backend::{api, serial, websocket, AppState};

/// 主函数
#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // 加载环境变量（必须在日志初始化之前，否则 .env 中的 RUST_LOG 不生效）
    dotenvy::dotenv().ok();

    // 初始化日志
    tracing_subscriber::fmt()
        .with_env_filter("info,esp_smart_car_backend=debug")
        .init();

    info!("智能车桌面端后端启动");
    info!("版本: 1.2.0");

    // 创建应用状态
    let state = Arc::new(AppState::new());

    // 启动串口通信任务（退出后自动重启，防止"假死"）
    let serial_state = state.clone();
    tokio::spawn(async move {
        loop {
            if let Err(e) = serial::run_serial_task(serial_state.clone()).await {
                warn!("串口任务错误: {}, 3秒后重启", e);
                tokio::time::sleep(tokio::time::Duration::from_secs(3)).await;
            } else {
                // 正常退出（如断开连接），短暂等待后重启
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
        .route("/api/ports", get(api::list_ports));

    // 静态文件路由（SPA fallback）
    let static_routes = Router::new().fallback_service(
        tower_http::services::ServeDir::new("./frontend/dist").fallback(
            tower_http::services::ServeFile::new("./frontend/dist/index.html"),
        ),
    );

    let app = Router::new()
        .route("/ws", get(websocket::ws_handler))
        .merge(api_routes)
        .merge(static_routes)
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

#[cfg(test)]
mod tests {
    use esp_smart_car_backend::AppState;

    /// 测试 AppState 初始状态
    #[tokio::test]
    async fn test_app_state_new() {
        let state = AppState::new();
        let current_speed = state
            .current_speed
            .load(std::sync::atomic::Ordering::Relaxed);
        assert_eq!(current_speed, 5);
        let video_frame = state.video_frame.lock().unwrap();
        assert!(video_frame.is_none());
    }
}
