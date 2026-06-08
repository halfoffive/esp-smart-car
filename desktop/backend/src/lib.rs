/**
 * 库入口
 * 重新导出所有公共模块和类型，供集成测试使用
 */
pub mod api;
pub mod serial;
pub mod websocket;

pub use serial::OdometryData;

use std::sync::Arc;
use tokio::sync::Mutex;

/// 应用状态（共享状态）
pub struct AppState {
    /// 串口连接管理器（使用 std::sync::Mutex，因为串口 I/O 是阻塞的）
    pub serial_manager: Arc<std::sync::Mutex<serial::SerialManager>>,
    /// WebSocket连接管理器
    pub ws_manager: Arc<Mutex<websocket::WebSocketManager>>,
    /// 视频帧数据
    pub video_frame: Arc<Mutex<Option<Vec<u8>>>>,
    /// 当前速度
    pub current_speed: Arc<Mutex<u8>>,
    /// 测速数据
    pub odometry: Arc<Mutex<OdometryData>>,
    /// 最后心跳时间
    pub last_heartbeat: Arc<Mutex<std::time::Instant>>,
    /// 服务器启动时间（用于计算运行时长）
    pub started_at: std::time::Instant,
}

impl Default for AppState {
    fn default() -> Self {
        Self::new()
    }
}

impl AppState {
    /// 创建新状态
    pub fn new() -> Self {
        Self {
            serial_manager: Arc::new(std::sync::Mutex::new(serial::SerialManager::new())),
            ws_manager: Arc::new(Mutex::new(websocket::WebSocketManager::new())),
            video_frame: Arc::new(Mutex::new(None)),
            current_speed: Arc::new(Mutex::new(5)),
            odometry: Arc::new(Mutex::new(OdometryData::default())),
            last_heartbeat: Arc::new(Mutex::new(std::time::Instant::now())),
            started_at: std::time::Instant::now(),
        }
    }
}
