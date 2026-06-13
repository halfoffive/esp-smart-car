/**
 * 库入口
 * 重新导出所有公共模块和类型，供集成测试使用
 */
pub mod api;
pub mod serial;
pub mod websocket;

pub use serial::OdometryData;

/// BLE 设备信息
#[derive(Debug, Clone)]
pub struct BleDevice {
    /// 设备名称
    pub name: String,
    /// MAC 地址
    pub mac: String,
    /// 信号强度
    pub rssi: i16,
}

use std::sync::atomic::AtomicU8;
use std::sync::Arc;

/// 应用状态（共享状态）
pub struct AppState {
    /// 串口连接管理器（使用 std::sync::Mutex，因为串口 I/O 是阻塞的）
    pub serial_manager: Arc<std::sync::Mutex<serial::SerialManager>>,
    /// WebSocket连接管理器（使用 std::sync::Mutex，操作均为内存操作，不跨 .await 持锁）
    pub ws_manager: Arc<std::sync::Mutex<websocket::WebSocketManager>>,
    /// 视频帧数据（使用 std::sync::Mutex，不跨 .await 持锁）
    /// 内层 Arc 共享引用，避免 clone 整帧数据
    pub video_frame: Arc<std::sync::Mutex<Option<Arc<Vec<u8>>>>>,
    /// 视频帧 Base64 编码数据（共享引用，避免每客户端重复编码）
    pub video_frame_b64: Arc<std::sync::Mutex<Option<Arc<String>>>>,

    /// 当前速度（使用 AtomicU8，单字节无锁原子操作）
    pub current_speed: AtomicU8,
    /// 测速数据（使用 std::sync::Mutex，不跨 .await 持锁）
    pub odometry: Arc<std::sync::Mutex<OdometryData>>,
    /// 最后心跳时间（使用 std::sync::Mutex，不跨 .await 持锁）
    pub last_heartbeat: Arc<std::sync::Mutex<std::time::Instant>>,
    /// 服务器启动时间（用于计算运行时长）
    pub started_at: std::time::Instant,
    /// 可用串口列表（使用 tokio::sync::Mutex，供 async 端点读取）
    pub available_ports: Arc<tokio::sync::Mutex<Vec<String>>>,
    /// 上一次扫描到的串口列表（使用 std::sync::Mutex，不跨 .await 持锁）
    pub last_ports: Arc<std::sync::Mutex<Vec<String>>>,
    /// BLE 设备列表（使用 std::sync::Mutex，不跨 .await 持锁）
    pub ble_devices: Arc<std::sync::Mutex<Vec<BleDevice>>>,
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
            ws_manager: Arc::new(std::sync::Mutex::new(websocket::WebSocketManager::new())),
            video_frame: Arc::new(std::sync::Mutex::new(None)),
            video_frame_b64: Arc::new(std::sync::Mutex::new(None)),

            current_speed: AtomicU8::new(5),
            odometry: Arc::new(std::sync::Mutex::new(OdometryData::default())),
            last_heartbeat: Arc::new(std::sync::Mutex::new(std::time::Instant::now())),
            started_at: std::time::Instant::now(),
            available_ports: Arc::new(tokio::sync::Mutex::new(Vec::new())),
            last_ports: Arc::new(std::sync::Mutex::new(Vec::new())),
            ble_devices: Arc::new(std::sync::Mutex::new(Vec::new())),
        }
    }
}
