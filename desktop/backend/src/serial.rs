/**
 * 串口通信模块
 * 基于 serialport 库，实现与 ESP32 接收器的通信
 *
 * 功能：
 * 1. 扫描可用串口
 * 2. 连接/断开串口
 * 3. 发送控制命令
 * 4. 接收视频帧数据
 * 5. 解析数据帧
 *
 * 数据格式：
 * 发送：单字节命令（W/A/S/D/1-9/空格等）
 * 接收：[0xAA][0x55][帧大小(4字节)][帧数据]
 */
use std::io::{Read, Write};
use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use serialport::{SerialPort, SerialPortType};
use tracing::{debug, error, info, warn};

use crate::AppState;

/// 测速数据
#[derive(Debug, Clone)]
pub struct OdometryData {
    /// 左轮速度(mm/s)
    pub left_speed_mmps: f32,
    /// 右轮速度(mm/s)
    pub right_speed_mmps: f32,
    /// 航向角(弧度)
    pub heading: f32,
    /// 总行走距离(mm)
    pub total_distance_mm: f32,
    /// 最后更新时间
    pub last_update: std::time::Instant,
}

impl Default for OdometryData {
    fn default() -> Self {
        Self {
            left_speed_mmps: 0.0,
            right_speed_mmps: 0.0,
            heading: 0.0,
            total_distance_mm: 0.0,
            last_update: std::time::Instant::now(),
        }
    }
}

/// 串口帧头
const FRAME_HEADER: [u8; 2] = [0xAA, 0x55];
/// 默认波特率
pub const DEFAULT_BAUD_RATE: u32 = 921_600;
/// 读取超时
const READ_TIMEOUT: Duration = Duration::from_millis(100);

/// 串口连接状态
#[derive(Debug, Clone)]
pub enum SerialConnectionState {
    /// 未连接
    Disconnected,
    /// 连接中
    Connecting,
    /// 已连接
    Connected { port_name: String, baud_rate: u32 },
    /// 连接错误
    Error(String),
}

/// 串口管理器
pub struct SerialManager {
    /// 当前串口连接
    port: Option<Box<dyn SerialPort>>,
    /// 连接状态
    pub state: SerialConnectionState,
    /// 已接收的帧数
    pub frame_count: u32,
    /// 已发送的字节数
    pub bytes_sent: u64,
    /// 已发送的命令数
    pub command_count: u64,
    /// 行缓冲区（用于解析测速JSON行）
    line_buffer: Vec<u8>,
}

impl Default for SerialManager {
    fn default() -> Self {
        Self::new()
    }
}

impl SerialManager {
    /// 创建新管理器
    pub fn new() -> Self {
        Self {
            port: None,
            state: SerialConnectionState::Disconnected,
            frame_count: 0,
            bytes_sent: 0,
            command_count: 0,
            line_buffer: Vec::new(),
        }
    }

    /// 扫描可用串口
    pub fn list_ports() -> Vec<(String, String)> {
        let mut ports = Vec::new();

        match serialport::available_ports() {
            Ok(available_ports) => {
                for port in available_ports {
                    let info = match &port.port_type {
                        SerialPortType::UsbPort(info) => {
                            format!(
                                "{} {} (VID:{:04X} PID:{:04X})",
                                info.manufacturer.as_deref().unwrap_or("Unknown"),
                                info.product.as_deref().unwrap_or("Unknown"),
                                info.vid,
                                info.pid
                            )
                        }
                        SerialPortType::BluetoothPort => "Bluetooth".to_string(),
                        SerialPortType::PciPort => "PCI".to_string(),
                        SerialPortType::Unknown => "Unknown".to_string(),
                        #[allow(unreachable_patterns)]
                        _ => "Other".to_string(),
                    };
                    ports.push((port.port_name, info));
                }
            }
            Err(e) => {
                warn!("扫描串口失败: {}", e);
            }
        }

        ports
    }

    /// 连接串口
    pub fn connect(&mut self, port_name: &str, baud_rate: u32) -> Result<()> {
        info!("连接串口: {} @ {} 波特", port_name, baud_rate);

        self.state = SerialConnectionState::Connecting;

        let port = serialport::new(port_name, baud_rate)
            .timeout(READ_TIMEOUT)
            .data_bits(serialport::DataBits::Eight)
            .parity(serialport::Parity::None)
            .stop_bits(serialport::StopBits::One)
            .flow_control(serialport::FlowControl::None)
            .open()?;

        self.port = Some(port);
        self.state = SerialConnectionState::Connected {
            port_name: port_name.to_string(),
            baud_rate,
        };

        info!("串口连接成功: {}", port_name);
        Ok(())
    }

    /// 断开串口
    pub fn disconnect(&mut self) {
        if self.port.is_some() {
            info!("断开串口连接");
            self.port = None;
            self.state = SerialConnectionState::Disconnected;
        }
    }

    /// 发送命令
    pub fn send_command(&mut self, cmd: u8) -> Result<()> {
        if let Some(ref mut port) = self.port {
            port.write_all(&[cmd])?;
            port.flush()?;
            self.bytes_sent += 1;
            self.command_count += 1;
            debug!("发送命令: 0x{:02X} ('{}')", cmd, cmd as char);
            Ok(())
        } else {
            Err(anyhow::anyhow!("串口未连接"))
        }
    }

    /// 读取串口行数据（非视频帧，测速JSON行）
    /// 返回完整行（去除换行符）
    pub fn read_line(&mut self) -> Option<String> {
        if let Some(ref mut port) = self.port {
            let mut byte = [0u8; 1];
            loop {
                match port.read_exact(&mut byte) {
                    Ok(()) => {
                        self.line_buffer.push(byte[0]);
                        // 检测换行符
                        if byte[0] == b'\n' {
                            // 去除可能的 \r\n
                            while self.line_buffer.last() == Some(&b'\n')
                                || self.line_buffer.last() == Some(&b'\r')
                            {
                                self.line_buffer.pop();
                            }
                            if !self.line_buffer.is_empty() {
                                let line = String::from_utf8_lossy(&self.line_buffer).to_string();
                                self.line_buffer.clear();
                                return Some(line);
                            }
                        }
                    }
                    Err(_) => {
                        // 超时或错误，返回当前缓冲区
                        return None;
                    }
                }
            }
        }
        None
    }

    /// 解析测速JSON行
    /// 格式: {"t":"odom","ls":左速度,"rs":右速度,"hd":航向*100,"dist":距离}
    pub fn parse_odometry_line(line: &str) -> Option<OdometryData> {
        if !line.contains("\"t\":\"odom\"") {
            return None;
        }

        let parsed: serde_json::Value = serde_json::from_str(line).ok()?;

        let left_speed = parsed["ls"].as_f64()? as f32;
        let right_speed = parsed["rs"].as_f64()? as f32;
        let heading_x100 = parsed["hd"].as_f64()? as f32;
        let total_dist = parsed["dist"].as_f64()? as f32;

        Some(OdometryData {
            left_speed_mmps: left_speed,
            right_speed_mmps: right_speed,
            heading: heading_x100 / 100.0,
            total_distance_mm: total_dist,
            last_update: std::time::Instant::now(),
        })
    }

    /// 读取视频帧
    pub fn read_video_frame(&mut self, buffer: &mut Vec<u8>) -> Result<bool> {
        if let Some(ref mut port) = self.port {
            let mut header = [0u8; 2];

            // 查找帧头
            let mut found = false;
            for _ in 0..1000 {
                if let Ok(()) = port.read_exact(&mut header[0..1]) {
                    if header[0] == FRAME_HEADER[0] {
                        if let Ok(()) = port.read_exact(&mut header[1..2]) {
                            if header[1] == FRAME_HEADER[1] {
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }

            if !found {
                return Ok(false);
            }

            // 读取帧大小（4字节）
            let mut size_bytes = [0u8; 4];
            port.read_exact(&mut size_bytes)?;
            let frame_size = u32::from_le_bytes(size_bytes) as usize;

            // 检查帧大小是否合理
            if frame_size > 10 * 1024 * 1024 || frame_size == 0 {
                warn!("帧大小异常: {} 字节", frame_size);
                return Ok(false);
            }

            // 读取帧数据
            buffer.resize(frame_size, 0);
            port.read_exact(buffer)?;

            self.frame_count += 1;
            debug!("接收帧 #{}: {} 字节", self.frame_count, frame_size);

            Ok(true)
        } else {
            Ok(false)
        }
    }
}

/// 串口任务结果（用于 spawn_blocking 与 async 上下文间传递数据）
enum SerialTaskResult {
    /// 读取到视频帧
    VideoFrame(Vec<u8>),
    /// 读取到测速数据行
    OdometryLine(String),
    /// 无数据
    NoData,
    /// 错误
    Error(String),
}

/// 串口通信任务（在独立线程中运行）
pub async fn run_serial_task(state: std::sync::Arc<AppState>) -> Result<()> {
    info!("串口通信任务启动");

    let mut frame_buffer = Vec::new();

    loop {
        // 检查串口是否连接
        let is_connected = {
            let manager = state.serial_manager.lock().unwrap();
            matches!(manager.state, SerialConnectionState::Connected { .. })
        };

        if is_connected {
            let state_clone = Arc::clone(&state);
            // 使用 std::mem::take 获取所有权，避免不必要的 clone
            let mut local_buffer = std::mem::take(&mut frame_buffer);

            // 在 spawn_blocking 中执行阻塞 I/O，避免阻塞 Tokio 运行时
            let result = tokio::task::spawn_blocking(move || {
                let mut manager = state_clone.serial_manager.lock().unwrap();

                // 读取视频帧
                match manager.read_video_frame(&mut local_buffer) {
                    Ok(true) => SerialTaskResult::VideoFrame(local_buffer),
                    Ok(false) => {
                        // 尝试读取测速数据行
                        if let Some(line) = manager.read_line() {
                            SerialTaskResult::OdometryLine(line)
                        } else {
                            SerialTaskResult::NoData
                        }
                    }
                    Err(e) => SerialTaskResult::Error(e.to_string()),
                }
            })
            .await;

            match result {
                Ok(SerialTaskResult::VideoFrame(buffer)) => {
                    // 帧数据使用 std::mem::take 避免再次 clone
                    frame_buffer = buffer;
                    let frame_data = frame_buffer.clone();
                    // serial_manager 锁已释放，单独获取 video_frame 锁
                    let mut video = state.video_frame.lock().await;
                    *video = Some(frame_data);
                }
                Ok(SerialTaskResult::OdometryLine(line)) => {
                    // serial_manager 锁已释放，单独获取 odometry 锁
                    if let Some(odom_data) = SerialManager::parse_odometry_line(&line) {
                        let mut odom = state.odometry.lock().await;
                        *odom = odom_data;
                        debug!(
                            "测速数据: 左={}mm/s, 右={}mm/s, 航向={}rad",
                            odom.left_speed_mmps as f64,
                            odom.right_speed_mmps as f64,
                            odom.heading as f64
                        );
                    }
                }
                Ok(SerialTaskResult::NoData) => {
                    // 无数据，短暂等待
                    tokio::time::sleep(Duration::from_millis(10)).await;
                }
                Ok(SerialTaskResult::Error(e)) => {
                    error!("串口读取错误: {}", e);
                    let mut manager = state.serial_manager.lock().unwrap();
                    manager.disconnect();
                }
                Err(e) if e.is_panic() => {
                    // spawn_blocking 任务 panic，记录详细信息
                    error!("串口任务 panic: {:?}，可能需要重启", e);
                    let mut manager = state.serial_manager.lock().unwrap();
                    manager.disconnect();
                }
                Err(e) if e.is_cancelled() => {
                    // 任务被正常取消，静默处理
                    debug!("串口阻塞任务被取消");
                }
                Err(e) => {
                    // 其他未知 JoinError
                    warn!("串口任务执行错误: {}", e);
                }
            }
        } else {
            // 未连接，等待
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 测试 SerialManager 初始状态
    #[test]
    fn test_serial_manager_new() {
        let manager = SerialManager::new();
        assert!(matches!(manager.state, SerialConnectionState::Disconnected));
        assert_eq!(manager.frame_count, 0);
        assert_eq!(manager.bytes_sent, 0);
        assert_eq!(manager.command_count, 0);
    }

    /// 测试未连接时发送命令返回错误
    #[test]
    fn test_send_command_disconnected() {
        let mut manager = SerialManager::new();
        let result = manager.send_command(0x57);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("串口未连接"));
    }

    /// 测试未连接时断开无 panic
    #[test]
    fn test_disconnect_when_disconnected() {
        let mut manager = SerialManager::new();
        manager.disconnect();
        assert!(matches!(manager.state, SerialConnectionState::Disconnected));
    }

    /// 测试测速 JSON 解析 - 有效数据
    #[test]
    fn test_parse_odometry_line_valid() {
        let line = r#"{"t":"odom","ls":100.5,"rs":99.3,"hd":18000,"dist":12345}"#;
        let odom = SerialManager::parse_odometry_line(line).expect("解析有效测速 JSON 失败");
        assert!((odom.left_speed_mmps - 100.5).abs() < 0.1);
        assert!((odom.right_speed_mmps - 99.3).abs() < 0.1);
        assert!((odom.heading - 180.0).abs() < 0.1);
        assert!((odom.total_distance_mm - 12345.0).abs() < 0.1);
    }

    /// 测试测速 JSON 解析 - 非 odom 消息
    #[test]
    fn test_parse_odometry_line_not_odom() {
        let line = r#"{"t":"other","ls":100}"#;
        assert!(SerialManager::parse_odometry_line(line).is_none());
    }

    /// 测试测速 JSON 解析 - 无效 JSON
    #[test]
    fn test_parse_odometry_line_invalid_json() {
        assert!(SerialManager::parse_odometry_line("not json").is_none());
    }

    /// 测试测速 JSON 解析 - 缺少字段
    #[test]
    fn test_parse_odometry_line_missing_fields() {
        let line = r#"{"t":"odom","ls":100}"#;
        assert!(SerialManager::parse_odometry_line(line).is_none());
    }

    /// 测试 OdometryData 默认值
    #[test]
    fn test_odometry_default() {
        let odom = OdometryData::default();
        assert!((odom.left_speed_mmps - 0.0).abs() < f32::EPSILON);
        assert!((odom.right_speed_mmps - 0.0).abs() < f32::EPSILON);
        assert!((odom.heading - 0.0).abs() < f32::EPSILON);
        assert!((odom.total_distance_mm - 0.0).abs() < f32::EPSILON);
    }
}
