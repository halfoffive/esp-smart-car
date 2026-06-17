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
use std::io::{BufReader, Read, Write};
use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use base64::Engine;
use serialport::{SerialPort, SerialPortType};
use tracing::{debug, error, info, warn};

use crate::AppState;

/// 测速数据
#[derive(Debug)]
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

impl Clone for OdometryData {
    fn clone(&self) -> Self {
        Self {
            left_speed_mmps: self.left_speed_mmps,
            right_speed_mmps: self.right_speed_mmps,
            heading: self.heading,
            total_distance_mm: self.total_distance_mm,
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

/// 串口读取结果（统一缓冲状态机）
pub enum SerialReadResult {
    /// 读取到视频帧
    VideoFrame(Vec<u8>),
    /// 读取到测速数据行
    OdometryLine(String),
    /// 无数据（超时）
    NoData,
}

/// 串口管理器
pub struct SerialManager {
    /// 当前串口连接（使用 BufReader 缓冲读取，解决帧头重叠遗漏问题）
    port: Option<BufReader<Box<dyn SerialPort>>>,
    /// 串口写句柄（通过 try_clone 创建的独立句柄，与 port 可并发读写）
    write_port: Option<Box<dyn SerialPort>>,
    /// 连接状态
    pub state: SerialConnectionState,
    /// 已接收的帧数
    pub frame_count: u32,
    /// 已发送的字节数
    pub bytes_sent: u64,
    /// 已发送的命令数
    pub command_count: u64,
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
            write_port: None,
            state: SerialConnectionState::Disconnected,
            frame_count: 0,
            bytes_sent: 0,
            command_count: 0,
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

        let port = match serialport::new(port_name, baud_rate)
            .timeout(READ_TIMEOUT)
            .data_bits(serialport::DataBits::Eight)
            .parity(serialport::Parity::None)
            .stop_bits(serialport::StopBits::One)
            .flow_control(serialport::FlowControl::None)
            .open()
        {
            Ok(p) => p,
            Err(e) => {
                // 连接失败时恢复状态为 Disconnected，避免永远卡在 Connecting
                self.state = SerialConnectionState::Disconnected;
                return Err(e.into());
            }
        };

        // 克隆写句柄：串口支持 try_clone，读写可并发操作不同句柄
        let write_port = match port.try_clone() {
            Ok(wp) => wp,
            Err(e) => {
                self.state = SerialConnectionState::Disconnected;
                return Err(anyhow::anyhow!("串口写句柄克隆失败: {}", e));
            }
        };

        self.write_port = Some(write_port);
        self.port = Some(BufReader::new(port));
        self.state = SerialConnectionState::Connected {
            port_name: port_name.to_string(),
            baud_rate,
        };

        info!("串口连接成功: {}", port_name);
        Ok(())
    }

    /// 断开串口
    /// 无条件清理 port 和状态，防止以下资源泄漏：
    /// - run_serial_task 通过 port.take() 临时取出 port 后，disconnect() 被调用，
    ///   若条件判断跳过清理，run_serial_task 归还 port 后将永远无法再访问到它
    /// - port 为 Some 但 state 不是 Connected 的异常状态
    pub fn disconnect(&mut self) {
        // 无条件清理：即使 port 当前被 run_serial_task 临时取出（为 None），
        // 也必须将 state 设为 Disconnected，这样 run_serial_task 归还 port 时
        // 能检测到 Disconnected 状态并 drop port，而不是将其放回已废弃的 manager
        info!("断开串口连接");
        self.port = None;
        self.write_port = None;
        self.state = SerialConnectionState::Disconnected;
    }

    /// 发送单字节命令
    pub fn send_command(&mut self, cmd: u8) -> Result<()> {
        self.send_bytes(&[cmd])
    }

    /// 发送多字节数据（使用独立写句柄，与读操作可并发）
    pub fn send_bytes(&mut self, data: &[u8]) -> Result<()> {
        if let Some(ref mut wp) = self.write_port {
            wp.write_all(data)?;
            wp.flush()?;
            self.bytes_sent += data.len() as u64;
            self.command_count += 1;
            debug!("发送数据: {} 字节", data.len());
            Ok(())
        } else if matches!(self.state, SerialConnectionState::Connected { .. }) {
            // write_port 为 None 但 state 为 Connected：异常状态（不应再发生）
            Err(anyhow::anyhow!("串口写句柄异常，请断开后重连"))
        } else {
            Err(anyhow::anyhow!("串口未连接"))
        }
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

    /// 解析 BLE 设备 JSON 行
    /// 格式: {"t":"ble","devices":[{"name":"xxx","mac":"AA:BB:CC:DD:EE:FF","rssi":-42,"wifi_mac":"AA:BB:CC:DD:EE:FF"},...]}
    /// wifi_mac 为可选项，仅当设备广播了 Manufacturer Data 且包含 WiFi MAC 时才会出现
    pub fn parse_ble_line(line: &str) -> Option<Vec<crate::BleDevice>> {
        if !line.contains("\"t\":\"ble\"") {
            return None;
        }

        let parsed: serde_json::Value = serde_json::from_str(line).ok()?;
        let devices_array = parsed.get("devices")?.as_array()?;

        let mut devices = Vec::new();
        for dev in devices_array {
            let name = dev.get("name")?.as_str()?.to_string();
            let mac = dev.get("mac")?.as_str()?.to_string();
            let rssi = dev.get("rssi")?.as_i64()? as i16;
            // wifi_mac 为可选项：仅车载 C6 等设备会广播
            let wifi_mac = dev.get("wifi_mac").and_then(|v| v.as_str()).map(String::from);
            devices.push(crate::BleDevice { name, mac, rssi, wifi_mac });
        }

        Some(devices)
    }

    /// 流对齐恢复：跳过字节直到找到下一个 0xAA 0x55 帧头
    fn resync_stream(port: &mut BufReader<Box<dyn SerialPort>>) -> Result<()> {
        let start = std::time::Instant::now();
        let mut prev_byte = 0u8;
        while start.elapsed() < Duration::from_secs(2) {
            let mut byte = [0u8; 1];
            match port.read(&mut byte) {
                Ok(0) => continue,
                Ok(_) => {
                    if prev_byte == FRAME_HEADER[0] && byte[0] == FRAME_HEADER[1] {
                        // 找到帧头，流已对齐
                        debug!("流对齐恢复成功");
                        return Ok(());
                    }
                    prev_byte = byte[0];
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => continue,
                Err(e) => return Err(anyhow::anyhow!("流对齐恢复时串口错误: {}", e)),
            }
        }
        warn!("流对齐恢复超时（2秒内未找到帧头）");
        Err(anyhow::anyhow!("流对齐恢复超时"))
    }

    /// 读取视频帧（帧头已确认后读取帧大小和数据）
    /// 独立函数，仅接收 port 参数，避免与 self.port 的可变借用冲突
    fn read_frame_data(
        port: &mut BufReader<Box<dyn SerialPort>>,
    ) -> Result<SerialReadResult> {
        let mut size_bytes = [0u8; 4];
        port.read_exact(&mut size_bytes)?;
        let frame_size = u32::from_le_bytes(size_bytes) as usize;
        if frame_size > 256 * 1024 || frame_size == 0 {
            warn!("帧大小异常: {} 字节，进入流对齐恢复", frame_size);
            // 帧大小异常，跳过数据直到找到下一个帧头
            Self::resync_stream(port)?;
            return Ok(SerialReadResult::NoData);
        }
        let mut frame_data = vec![0u8; frame_size];
        match port.read_exact(&mut frame_data) {
            Ok(()) => {
                // 验证帧数据是否为有效 JPEG（以 0xFF 0xD8 SOI 标记开头）
                // 防止串口数据中恰好出现 0xAA 0x55 字节序列导致的帧头误检测
                if frame_size >= 2 && frame_data[0] == 0xFF && frame_data[1] == 0xD8 {
                    debug!("接收帧: {} 字节", frame_size);
                    Ok(SerialReadResult::VideoFrame(frame_data))
                } else {
                    warn!("帧数据不以 JPEG SOI 开头（疑似帧头误检测），触发流对齐恢复");
                    Self::resync_stream(port)?;
                    Ok(SerialReadResult::NoData)
                }
            }
            Err(e) => {
                warn!("读取帧数据失败: {}，进入流对齐恢复", e);
                // 帧数据读取失败，跳过剩余字节直到找到下一个帧头
                Self::resync_stream(port)?;
                Ok(SerialReadResult::NoData)
            }
        }
    }

    /// 统一读取方法：同时处理视频帧和测速JSON行
    /// 解决帧头重叠遗漏和视频/测速数据互斥吞没问题
    /// 独立函数，不持有 SerialManager 锁，避免阻塞其他 API 请求
    pub fn read_next(port: &mut BufReader<Box<dyn SerialPort>>) -> Result<SerialReadResult> {
        // 策略：逐字节读取，维护一个行缓冲区
        // - 遇到 0xAA 时尝试匹配帧头 0xAA 0x55
        // - 如果 0xAA 后不是 0x55，只丢弃第一个 0xAA，保留第二个字节重新检查
        // - 遇到 \n 时，将行缓冲区内容作为测速数据返回
        // - 设置5秒总超时

        let start = std::time::Instant::now();
        let mut line_buf: Vec<u8> = Vec::new();

        while start.elapsed() < Duration::from_secs(5) {
            let mut byte = [0u8; 1];
            match port.read(&mut byte) {
                Ok(0) => continue,
                Ok(_) => {
                    let b = byte[0];

                    // 防止 line_buf 无限增长（虽已设5秒超时，加硬上限更安全）
                    if line_buf.len() > 64 * 1024 {
                        warn!("行缓冲区超过 64KB 上限，丢弃");
                        line_buf.clear();
                    }

                    if b == FRAME_HEADER[0] {
                        // 可能是帧头起始，尝试读取第二个字节
                        let mut second = [0u8; 1];
                        match port.read(&mut second) {
                            Ok(0) => {
                                // 只读到一个 0xAA，后面没有更多数据
                                // 将 0xAA 加入行缓冲区（可能是测速数据的一部分）
                                line_buf.push(b);
                                continue;
                            }
                            Ok(_) => {
                                if second[0] == FRAME_HEADER[1] {
                                    // 找到帧头 0xAA 0x55
                                    // 局限性：line_buf 中累积的文本数据（可能的测速JSON行）
                                    // 在此被丢弃。由于函数返回单一 SerialReadResult，无法同时返回
                                    // OdometryLine 和 VideoFrame。实际影响很小，因为测速数据以
                                    // \n 换行、视频帧以 0xAA 0x55 帧头分隔，二者不应混叠在同一个读取周期。
                                    let result = Self::read_frame_data(port);
                                    return result;
                                } else {
                                    // 0xAA 不是帧头（后跟非 0x55 字节），不作为帧头处理
                                    // 将 0xAA 和第二个字节作为普通数据保留在行缓冲中
                                    // 关键修复：不丢弃第二个字节
                                    line_buf.push(b); // 保留 0xAA
                                    // 重新处理第二个字节
                                    if second[0] == FRAME_HEADER[0] {
                                        // 第二个字节也是 0xAA，可能是帧头的开始
                                        // 尝试读取第三个字节
                                        let mut third = [0u8; 1];
                                        match port.read(&mut third) {
                                            Ok(0) => {
                                                line_buf.push(second[0]);
                                                continue;
                                            }
                                            Ok(_) => {
                                                if third[0] == FRAME_HEADER[1] {
                                                    // 找到 0xAA 0x55 帧头！
                                                    let result = Self::read_frame_data(port);
                                                    return result;
                                                } else {
                                                    // 0xAA 0xAA 0xXX (XX != 0x55)
                                                    line_buf.push(second[0]);
                                                    line_buf.push(third[0]);
                                                    continue;
                                                }
                                            }
                                            Err(ref e)
                                                if e.kind()
                                                    == std::io::ErrorKind::TimedOut =>
                                            {
                                                line_buf.push(second[0]);
                                                continue;
                                            }
                                            Err(e) => {
                                                return Err(anyhow::anyhow!(
                                                    "串口读取错误: {}",
                                                    e
                                                ));
                                            }
                                        }
                                    } else {
                                        line_buf.push(second[0]);
                                        continue;
                                    }
                                }
                            }
                            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {
                                // 读取超时，0xAA 后面没有更多数据
                                line_buf.push(b);
                                continue;
                            }
                            Err(e) => {
                                return Err(anyhow::anyhow!("串口读取错误: {}", e));
                            }
                        }
                    } else if b == b'\n' {
                        // 行结束，尝试解析为测速JSON
                        if !line_buf.is_empty() {
                            match String::from_utf8(std::mem::take(&mut line_buf)) {
                                Ok(line) => {
                                    return Ok(SerialReadResult::OdometryLine(line));
                                }
                                Err(_) => {
                                    // line_buf 已被 std::mem::take 清空，无需再 clear
                                    continue;
                                }
                            }
                        }
                    } else {
                        line_buf.push(b);
                    }
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {
                    // 读取超时，检查行缓冲区是否有数据
                    if !line_buf.is_empty() {
                        // 没有完整行，继续等待
                        continue;
                    }
                    return Ok(SerialReadResult::NoData);
                }
                Err(e) => {
                    return Err(anyhow::anyhow!("串口读取错误: {}", e));
                }
            }
        }

        // 超时
        Ok(SerialReadResult::NoData)
    }
}

/// 串口任务结果（用于 spawn_blocking 与 async 上下文间传递数据）
enum SerialTaskResult {
    /// 读取到视频帧
    VideoFrame(Vec<u8>),
    /// 读取到测速数据行（可能是测速或 BLE 设备数据）
    OdometryLine(String),
    /// 无数据
    NoData,
    /// 错误
    Error { msg: String },
}

/// 串口通信任务（在独立线程中运行）
pub async fn run_serial_task(state: std::sync::Arc<AppState>) -> Result<()> {
    info!("串口通信任务启动");

    loop {
        // 检查串口是否连接
        let is_connected = {
            let manager = state.serial_manager.lock().expect("serial_manager lock poisoned");
            matches!(manager.state, SerialConnectionState::Connected { .. })
        };

        if is_connected {
            let state_clone = Arc::clone(&state);

            // 在 spawn_blocking 中执行阻塞 I/O，避免阻塞 Tokio 运行时
            // port 由 read 任务独占（send_bytes 使用独立的 write_port），take 后做 I/O 再归还
            let result = tokio::task::spawn_blocking(move || {
                // 短暂获取锁，取出 port
                let mut port = {
                    let mut manager = state_clone.serial_manager.lock().expect("serial_manager lock poisoned");
                    match manager.port.take() {
                        Some(p) => p,
                        None => return SerialTaskResult::NoData,
                    }
                };

                // 不持锁执行长时间 I/O（最长5秒）
                let result = SerialManager::read_next(&mut port);

                // 归还 port 并更新帧计数
                // 检查 disconnect() 是否在 I/O 期间被调用：若已断开则丢弃 port
                {
                    let mut manager = state_clone.serial_manager.lock().expect("serial_manager lock poisoned");
                    if matches!(manager.state, SerialConnectionState::Disconnected) {
                        // disconnect() 已在 I/O 期间调用，drop port 避免资源泄漏
                        drop(port);
                    } else {
                        manager.port = Some(port);
                    }
                    if let Ok(SerialReadResult::VideoFrame(_)) = &result {
                        manager.frame_count += 1;
                    }
                }

                match result {
                    Ok(SerialReadResult::VideoFrame(data)) => {
                        SerialTaskResult::VideoFrame(data)
                    }
                    Ok(SerialReadResult::OdometryLine(line)) => {
                        SerialTaskResult::OdometryLine(line)
                    }
                    Ok(SerialReadResult::NoData) => SerialTaskResult::NoData,
                    Err(e) => SerialTaskResult::Error {
                        msg: e.to_string(),
                    },
                }
            })
            .await;

            match result {
                Ok(SerialTaskResult::VideoFrame(buffer)) => {
                    // Base64 编码视频帧，存储共享引用避免每客户端重复编码
                    let b64_data = base64::engine::general_purpose::STANDARD.encode(&buffer);
                    let b64_arc = Arc::new(b64_data);

                    // 使用 Arc::clone 共享视频帧引用，避免 clone 整帧数据
                    {
                        let mut video = state.video_frame.lock().expect("video_frame lock poisoned");
                        *video = Some(Arc::new(buffer));
                    }
                    // 存储 Base64 编码结果，供 WebSocket 客户端共享读取
                    {
                        let mut b64 = state.video_frame_b64.lock().expect("video_frame_b64 lock poisoned");
                        *b64 = Some(b64_arc);
                    }
                }
                Ok(SerialTaskResult::OdometryLine(line)) => {
                    // 先尝试解析为 BLE 设备列表
                    if let Some(ble_devs) = SerialManager::parse_ble_line(&line) {
                        let mut devices =
                            state.ble_devices.lock().expect("ble_devices lock poisoned");
                        *devices = ble_devs;
                        info!("BLE 设备列表已更新: {} 个设备", devices.len());
                    } else if let Some(odom_data) = SerialManager::parse_odometry_line(&line) {
                        // serial_manager 锁已释放，单独获取 odometry 锁
                        let mut odom = state.odometry.lock().expect("odometry lock poisoned");
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
                    tokio::time::sleep(Duration::from_millis(50)).await;
                }
                Ok(SerialTaskResult::Error { msg }) => {
                    error!("串口读取错误: {}", msg);
                    let mut manager = state.serial_manager.lock().expect("serial_manager lock poisoned");
                    manager.disconnect();
                }
                Err(e) if e.is_panic() => {
                    // spawn_blocking 任务 panic，记录详细信息
                    error!("串口任务 panic: {:?}，可能需要重启", e);
                    let mut manager = state.serial_manager.lock().expect("serial_manager lock poisoned");
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

/// 串口扫描任务（每 1 秒扫描一次可用串口，变化时更新状态）
pub async fn run_port_scan_task(state: std::sync::Arc<AppState>) {
    info!("串口扫描任务启动");

    loop {
        // 获取当前可用串口列表（仅提取端口名称）
        let new_ports: Vec<String> = SerialManager::list_ports()
            .into_iter()
            .map(|(name, _info)| name)
            .collect();

        // 与上次扫描结果比较
        let changed = {
            let last = state.last_ports.lock().expect("last_ports lock poisoned");
            last.as_slice() != new_ports.as_slice()
        };

        if changed {
            // 更新可用串口列表（async 锁）
            let mut available = state.available_ports.lock().await;
            *available = new_ports.clone();
            drop(available);

            // 更新上次扫描结果（sync 锁）
            let mut last = state.last_ports.lock().expect("last_ports lock poisoned");
            *last = new_ports;
            info!("可用串口列表已更新");
        }

        tokio::time::sleep(Duration::from_secs(1)).await;
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

    /// 测试 AppState 初始串口列表为空
    #[test]
    fn test_app_state_ports_initially_empty() {
        let state = crate::AppState::new();
        let available = state.available_ports.blocking_lock();
        assert!(available.is_empty(), "初始可用串口列表应为空");
        let last = state.last_ports.lock().expect("last_ports lock poisoned");
        assert!(last.is_empty(), "初始 last_ports 应为空");
    }

    /// 测试 BLE 设备 JSON 行解析 - 有效数据
    #[test]
    fn test_parse_ble_line_valid() {
        let line = r#"{"t":"ble","devices":[{"name":"ESP32-C6","mac":"AA:BB:CC:DD:EE:01","rssi":-42},{"name":"Unknown","mac":"AA:BB:CC:DD:EE:02","rssi":-85}]}"#;
        let devices =
            SerialManager::parse_ble_line(line).expect("解析有效 BLE JSON 失败");
        assert_eq!(devices.len(), 2);
        assert_eq!(devices[0].name, "ESP32-C6");
        assert_eq!(devices[0].mac, "AA:BB:CC:DD:EE:01");
        assert_eq!(devices[0].rssi, -42);
        assert_eq!(devices[1].name, "Unknown");
        assert_eq!(devices[1].rssi, -85);
    }

    /// 测试 BLE 设备 JSON 行解析 - 非 ble 消息
    #[test]
    fn test_parse_ble_line_not_ble() {
        let line = r#"{"t":"odom","ls":100}"#;
        assert!(SerialManager::parse_ble_line(line).is_none());
    }

    /// 测试 BLE 设备 JSON 行解析 - 空设备列表
    #[test]
    fn test_parse_ble_line_empty() {
        let line = r#"{"t":"ble","devices":[]}"#;
        let devices = SerialManager::parse_ble_line(line).expect("解析空 BLE 列表失败");
        assert!(devices.is_empty());
    }

    /// 测试 BLE 设备 JSON 行解析 - 无效 JSON
    #[test]
    fn test_parse_ble_line_invalid_json() {
        assert!(SerialManager::parse_ble_line("not json").is_none());
    }

    /// 测试 BLE 设备 JSON 行解析 - 缺少 devices 字段
    #[test]
    fn test_parse_ble_line_missing_devices() {
        let line = r#"{"t":"ble"}"#;
        assert!(SerialManager::parse_ble_line(line).is_none());
    }
}
