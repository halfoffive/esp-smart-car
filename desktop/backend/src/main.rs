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
    // 初始化日志
    tracing_subscriber::fmt()
        .with_env_filter("info,esp_smart_car_backend=debug")
        .init();

    info!("智能车桌面端后端启动");
    info!("版本: 1.2.0");

    // 加载环境变量
    dotenvy::dotenv().ok();

    // 创建应用状态
    let state = Arc::new(AppState::new());

    // 启动串口通信任务
    let serial_state = state.clone();
    tokio::spawn(async move {
        if let Err(e) = serial::run_serial_task(serial_state).await {
            warn!("串口任务错误: {}", e);
        }
    });

    // 构建路由
    let app = Router::new()
        // WebSocket端点
        .route("/ws", get(websocket::ws_handler))
        // REST API
        .route("/api/command", post(api::handle_command))
        .route("/api/status", get(api::get_status))
        .route("/api/connect", post(api::connect_serial))
        .route("/api/disconnect", post(api::disconnect_serial))
        // 静态文件（前端构建产物），支持SPA fallback到index.html
        .fallback_service(
            tower_http::services::ServeDir::new("./frontend/dist").fallback(
                tower_http::services::ServeFile::new("./frontend/dist/index.html"),
            ),
        )
        // 注入状态
        .with_state(state);

    // 监听地址
    let addr = SocketAddr::from(([0, 0, 0, 0], 8080));
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
        let current_speed = state.current_speed.lock().await;
        assert_eq!(*current_speed, 5);
        let video_frame = state.video_frame.lock().await;
        assert!(video_frame.is_none());
    }
}
