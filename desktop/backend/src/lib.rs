/**
 * 库入口
 * 重新导出所有公共模块和类型，供集成测试使用
 */
pub mod api;
pub mod serial;
pub mod websocket;

pub use serial::{LinkStatus, OdometryData};

/// BLE 设备信息
#[derive(Debug, Clone)]
pub struct BleDevice {
    /// 设备名称
    pub name: String,
    /// BLE MAC 地址（扫描到的广播地址）
    pub mac: String,
    /// WiFi (ESP-NOW) MAC 地址（从 Manufacturer Data 提取，仅车载 C6 等设备会广播）
    pub wifi_mac: Option<String>,
    /// 信号强度
    pub rssi: i16,
}

use std::collections::HashMap;
use std::sync::atomic::{AtomicU16, AtomicU8};
use std::sync::{Arc, Mutex, MutexGuard};
use tracing::{info, warn};

/// Mutex 中毒恢复扩展 trait
///
/// AGENTS.md 规范要求禁止使用 unwrap/expect。对于 Mutex，poison 时直接 panic 会导致
/// 单个线程的错误扩散为整个服务崩溃。此 trait 在 poison 时记录警告并恢复锁内的数据，
/// 让服务继续运行，同时保留诊断信息。
pub trait MutexExt<T> {
    /// 获取锁；若 Mutex 已中毒，记录警告并恢复内部数据
    fn lock_or_recover(&self, name: &str) -> MutexGuard<'_, T>;
}

impl<T> MutexExt<T> for Mutex<T> {
    fn lock_or_recover(&self, name: &str) -> MutexGuard<'_, T> {
        match self.lock() {
            Ok(guard) => guard,
            Err(poisoned) => {
                warn!("Mutex {} 已中毒，正在恢复", name);
                poisoned.into_inner()
            }
        }
    }
}

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
    /// 视频帧哈希值（共享，避免每客户端重复计算）
    pub video_frame_hash: Arc<std::sync::Mutex<Option<u64>>>,

    /// 当前速度 PWM 值（0-255，使用 AtomicU8 无锁原子操作）
    pub current_speed: AtomicU8,
    /// 二进制数据包序列号（用于 WirelessPacket 的 seq 字段）
    pub packet_seq: AtomicU16,
    /// 测速数据（使用 std::sync::Mutex，不跨 .await 持锁）
    pub odometry: Arc<std::sync::Mutex<OdometryData>>,
    /// 链路状态（Dongle ↔ 车载 ESP-NOW 配对状态、车载在线状态）
    pub link_status: Arc<std::sync::Mutex<LinkStatus>>,
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
    /// 命令转发日志节流状态（命令字节, 上次记录时间），相同命令 1 秒内只记一次
    pub last_cmd_log: Arc<std::sync::Mutex<(u8, std::time::Instant)>>,
    /// 错误日志节流状态（错误类别, 上次记录时间），相同错误 5 秒内只记一次
    pub last_error_log: Arc<std::sync::Mutex<HashMap<String, std::time::Instant>>>,
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
            video_frame_hash: Arc::new(std::sync::Mutex::new(None)),

            current_speed: AtomicU8::new(128),
            packet_seq: AtomicU16::new(0),
            odometry: Arc::new(std::sync::Mutex::new(OdometryData::default())),
            link_status: Arc::new(std::sync::Mutex::new(LinkStatus::default())),
            last_heartbeat: Arc::new(std::sync::Mutex::new(std::time::Instant::now())),
            started_at: std::time::Instant::now(),
            available_ports: Arc::new(tokio::sync::Mutex::new(Vec::new())),
            last_ports: Arc::new(std::sync::Mutex::new(Vec::new())),
            ble_devices: Arc::new(std::sync::Mutex::new(Vec::new())),
            last_cmd_log: Arc::new(std::sync::Mutex::new((
                0,
                std::time::Instant::now()
                    .checked_sub(std::time::Duration::from_secs(10))
                    .unwrap_or_else(std::time::Instant::now),
            ))),
            last_error_log: Arc::new(std::sync::Mutex::new(HashMap::new())),
        }
    }

    /// 节流式命令转发日志：相同命令 1 秒内只记一次
    pub fn log_command_forward(&self, cmd: u8) {
        let mut last = self.last_cmd_log.lock_or_recover("last_cmd_log");
        let now = std::time::Instant::now();
        let should_log =
            last.0 != cmd || now.duration_since(last.1) >= std::time::Duration::from_secs(1);
        if should_log {
            *last = (cmd, now);
            info!("转发命令: {:?}", cmd as char);
        }
    }

    /// 节流式警告日志：相同错误类别 5 秒内只记一次
    pub fn warn_throttled(&self, category: &str, msg: String) {
        let mut last_errors = self.last_error_log.lock_or_recover("last_error_log");
        let now = std::time::Instant::now();
        let should_log = last_errors
            .get(category)
            .map(|&t| now.duration_since(t) >= std::time::Duration::from_secs(5))
            .unwrap_or(true);
        if should_log {
            last_errors.insert(category.to_string(), now);
            warn!("{}", msg);
        }
    }
}
