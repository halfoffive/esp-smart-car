/**
 * 智能车桌面端后端 - 主程序
 * 基于 Rust + Axum + WebSocket
 * 
 * 功能：
 * 1. HTTP Web服务器（提供静态文件和API）
 * 2. WebSocket（实时视频传输和命令）
 * 3. 串口通信（与ESP32接收器通信）
 * 
 * 架构：
 * - HTTP: 提供前端静态文件和REST API
 * - WebSocket: 双向实时通信（视频流 + 控制命令）
 * - Serial: 与ESP32接收器通过USB串口通信
 * 
 * 作者：智能车项目团队
 * 版本：1.0.0
 */

mod serial;
mod websocket;
mod api;

use std::net::SocketAddr;
use std::sync::Arc;

use axum::{
    routing::{get, post},
    Router,
};
use tokio::sync::Mutex;
use tracing::{info, warn};

/// 应用状态（共享状态）
pub struct AppState {
    /// 串口连接管理器
    pub serial_manager: Arc<Mutex<serial::SerialManager>>,
    /// WebSocket连接管理器
    pub ws_manager: Arc<Mutex<websocket::WebSocketManager>>,
    /// 视频帧数据
    pub video_frame: Arc<Mutex<Option<Vec<u8>>>>,
    /// 当前速度
    pub current_speed: Arc<Mutex<u8>>,
    /// 最后心跳时间
    pub last_heartbeat: Arc<Mutex<std::time::Instant>>,
    /// 服务器启动时间（用于计算运行时长）
    pub started_at: std::time::Instant,
}

impl AppState {
    /// 创建新状态
    pub fn new() -> Self {
        Self {
            serial_manager: Arc::new(Mutex::new(serial::SerialManager::new())),
            ws_manager: Arc::new(Mutex::new(websocket::WebSocketManager::new())),
            video_frame: Arc::new(Mutex::new(None)),
            current_speed: Arc::new(Mutex::new(128)),
            last_heartbeat: Arc::new(Mutex::new(std::time::Instant::now())),
            started_at: std::time::Instant::now(),
        }
    }
}

/// 主函数
#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // 初始化日志
    tracing_subscriber::fmt()
        .with_env_filter("info,esp_smart_car_backend=debug")
        .init();
    
    info!("智能车桌面端后端启动");
    info!("版本: 1.0.0");
    
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
        // 静态文件（前端构建产物）
        .nest_service("/", tower_http::services::ServeDir::new("./frontend/dist"))
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
