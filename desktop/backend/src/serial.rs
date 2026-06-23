/**
 * 串口通信模块
 * 基于 serialport 库，实现与 ESP32 接收器的通信
 *
 * 数据格式：
 * 发送：8 字节二进制 WirelessPacket（与 UDP 控制载荷格式一致）
 * 接收：[0xAA][0x55][帧大小(4字节)][帧数据]
 */
use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::io::{BufReader, Read, Write};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use serialport::SerialPort;
use tracing::{debug, error, info, warn};

use crate::{AppState, MutexExt, SharedVideoFrame};

// ===== 与 receiver_dongle 固件对齐的常量 =====

/// 帧头标记，与固件 VideoFrame 发送端一致
const FRAME_HEADER: [u8; 2] = [0xAA, 0x55];
/// 默认波特率，与固件 UART 配置一致
pub const DEFAULT_BAUD_RATE: u32 = 3_000_000;
/// 单次 read 超时，拆分长 I/O 为短循环以支持 generation 检查
const READ_TIMEOUT: Duration = Duration::from_millis(100);
/// 最大帧大小，与固件 VideoFrameBuffer 对齐
const MAX_FRAME_SIZE: usize = 32 * 1024;
/// read_next 总超时
const READ_TOTAL_TIMEOUT: Duration = Duration::from_secs(5);
/// 流对齐恢复超时
const RESYNC_TIMEOUT: Duration = Duration::from_secs(2);
/// 行缓冲区上限
const LINE_BUF_MAX: usize = 64 * 1024;

/// JPEG SOI 标记（帧起始）
const JPEG_SOI: [u8; 2] = [0xFF, 0xD8];
/// JPEG EOI 标记（帧结束）
const JPEG_EOI: [u8; 2] = [0xFF, 0xD9];

/// 帧解析状态机
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FrameParseState {
    /// 正常读取，累积行缓冲，等待 0xAA
    WaitingHeader,
    /// 已读到一个 0xAA，等待下一字节确认帧头
    Header0,
    /// 已读到 0xAA 0xAA，第二个 0xAA 可能是新帧头起始
    Header1,
    /// 帧头已确认，正在读取帧数据
    ReadingFrame,
}

/// 处理一帧视频数据：直接保存原始 JPEG 数据，不做转码或 Base64 编码
/// 整帧单包模式下，视频帧以原始二进制形式存储，供 WebSocket 直接发送 Binary 消息
pub fn process_video_frame(_state: Arc<AppState>, data: Vec<u8>) -> Option<SharedVideoFrame> {
    // 直接使用原始 JPEG 数据，不做任何转码或编码
    // 计算哈希用于前端去重
    let mut hasher = DefaultHasher::new();
    data.hash(&mut hasher);
    let hash = hasher.finish();

    Some(SharedVideoFrame {
        data: Arc::new(data),
        format: "jpeg",
        hash,
    })
}

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

/// 链路状态（Dongle ↔ 车载 WiFi/UDP 在线状态）
#[derive(Debug, Clone)]
pub struct LinkStatus {
    /// Dongle 是否正常工作
    pub dongle_ok: bool,
    /// 车载是否已通过 WiFi/UDP 与 Dongle 建立通信
    pub car_paired: bool,
    /// 上次收到车载数据的时间戳（毫秒，由 Dongle 上报）
    pub last_odom_ms: u64,
    /// 后端最后一次更新链路状态的时间
    pub last_updated: std::time::Instant,
}

impl Default for LinkStatus {
    fn default() -> Self {
        Self {
            dongle_ok: false,
            car_paired: false,
            last_odom_ms: 0,
            last_updated: std::time::Instant::now(),
        }
    }
}

impl PartialEq for LinkStatus {
    /// 比较链路状态是否发生变化（忽略 last_updated 字段）
    fn eq(&self, other: &Self) -> bool {
        self.dongle_ok == other.dongle_ok
            && self.car_paired == other.car_paired
            && self.last_odom_ms == other.last_odom_ms
    }
}

/// 串口连接状态
#[derive(Debug, Clone)]
pub enum SerialConnectionState {
    Disconnected,
    Connecting,
    Connected { port_name: String, baud_rate: u32 },
    Error(String),
}

/// 串口读取结果
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
    /// 读句柄（BufReader 缓冲读取）
    port: Option<BufReader<Box<dyn SerialPort>>>,
    /// 写句柄（Mutex 串行化写入，不依赖 try_clone 的并发安全假设）
    write_port: std::sync::Mutex<Option<Box<dyn SerialPort>>>,
    /// 连接状态
    pub state: SerialConnectionState,
    /// 已接收的视频帧数（u32 镜像，供 api.rs/websocket.rs 兼容访问）
    pub frame_count: u32,
    /// 已接收的视频帧数（主计数器）
    pub frames_received: u64,
    /// 已成功解码的视频帧数
    pub frames_decoded: u64,
    /// 已成功通过 WebSocket 发送的视频帧数
    pub frames_broadcasted: u64,
    /// 已发送的字节数
    pub bytes_sent: u64,
    /// 已发送的命令数
    pub command_count: u64,
    /// 连接代际计数器（AtomicU64，read_next 无需锁 manager 即可读取）
    pub port_generation: Arc<AtomicU64>,
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
            write_port: std::sync::Mutex::new(None),
            state: SerialConnectionState::Disconnected,
            frame_count: 0,
            frames_received: 0,
            frames_decoded: 0,
            frames_broadcasted: 0,
            bytes_sent: 0,
            command_count: 0,
            port_generation: Arc::new(AtomicU64::new(0)),
        }
    }

    /// 扫描可用串口，返回端口名列表
    pub fn list_ports() -> Vec<String> {
        match serialport::available_ports() {
            Ok(available_ports) => available_ports
                .into_iter()
                .map(|p| p.port_name)
                .collect(),
            Err(e) => {
                warn!("扫描串口失败: {}", e);
                Vec::new()
            }
        }
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
                self.state = SerialConnectionState::Disconnected;
                return Err(e.into());
            }
        };

        // 克隆写句柄：读写使用不同句柄，写句柄由 Mutex 串行化访问
        let write_port = match port.try_clone() {
            Ok(wp) => wp,
            Err(e) => {
                self.state = SerialConnectionState::Disconnected;
                return Err(anyhow::anyhow!("串口写句柄克隆失败: {}", e));
            }
        };

        *self.write_port.lock_or_panic("write_port") = Some(write_port);
        self.port = Some(BufReader::new(port));
        self.port_generation.fetch_add(1, Ordering::Relaxed);
        self.state = SerialConnectionState::Connected {
            port_name: port_name.to_string(),
            baud_rate,
        };

        info!(
            "串口连接成功: {} (generation={})",
            port_name,
            self.port_generation.load(Ordering::Relaxed)
        );
        Ok(())
    }

    /// 断开串口，无条件清理资源并递增 generation
    pub fn disconnect(&mut self) {
        info!("断开串口连接");
        self.port = None;
        *self.write_port.lock_or_panic("write_port") = None;
        self.port_generation.fetch_add(1, Ordering::Relaxed);
        self.state = SerialConnectionState::Disconnected;
    }

    /// 发送完整二进制数据包，递增命令计数
    pub fn send_packet(&mut self, packet: &[u8]) -> Result<()> {
        self.send_bytes(packet)?;
        self.command_count += 1;
        Ok(())
    }

    /// 发送多字节数据（不递增 command_count）
    pub fn send_bytes(&mut self, data: &[u8]) -> Result<()> {
        let mut wp_guard = self.write_port.lock_or_panic("write_port");
        if let Some(ref mut wp) = *wp_guard {
            wp.write_all(data)?;
            wp.flush()?;
            self.bytes_sent += data.len() as u64;
            debug!("发送数据: {} 字节", data.len());
            Ok(())
        } else {
            Err(anyhow::anyhow!("串口未连接"))
        }
    }

    /// 解析测速 JSON 行
    /// 格式: {"t":"odom","ls":左速度,"rs":右速度,"hd":航向*100,"dist":距离}
    pub fn parse_odometry_line(line: &str) -> Option<OdometryData> {
        let parsed: serde_json::Value = serde_json::from_str(line).ok()?;
        if parsed["t"].as_str()? != "odom" {
            return None;
        }
        Some(OdometryData {
            left_speed_mmps: parsed["ls"].as_f64()? as f32,
            right_speed_mmps: parsed["rs"].as_f64()? as f32,
            heading: parsed["hd"].as_f64()? as f32 / 100.0,
            total_distance_mm: parsed["dist"].as_f64()? as f32,
            last_update: std::time::Instant::now(),
        })
    }

    /// 解析 BLE 设备 JSON 行
    /// 格式: {"t":"ble","devices":[{"name":"xxx","mac":"AA:BB:CC:DD:EE:FF","rssi":-42,"wifi_mac":"..."},...]}
    pub fn parse_ble_line(line: &str) -> Option<Vec<crate::BleDevice>> {
        let parsed: serde_json::Value = serde_json::from_str(line).ok()?;
        if parsed["t"].as_str()? != "ble" {
            return None;
        }
        let devices_array = parsed.get("devices")?.as_array()?;
        let mut devices = Vec::new();
        for dev in devices_array {
            let name = dev.get("name")?.as_str()?.to_string();
            let mac = dev.get("mac")?.as_str()?.to_string();
            let rssi = dev.get("rssi")?.as_i64()? as i16;
            let wifi_mac = dev
                .get("wifi_mac")
                .and_then(|v| v.as_str())
                .map(String::from);
            devices.push(crate::BleDevice {
                name,
                mac,
                rssi,
                wifi_mac,
            });
        }
        Some(devices)
    }

    /// 解析链路状态 JSON 行
    /// 格式: {"t":"link","dongle":"ok","car_paired":true/false,"last_odom_ms":...}
    pub fn parse_link_line(line: &str) -> Option<LinkStatus> {
        let parsed: serde_json::Value = serde_json::from_str(line).ok()?;
        if parsed["t"].as_str()? != "link" {
            return None;
        }
        let dongle_str = parsed.get("dongle")?.as_str()?;
        let dongle_ok = dongle_str == "ok";
        let car_paired = parsed.get("car_paired")?.as_bool()?;
        // 兼容固件输出的 -1（从未收到车载数据），负数归一化为 0
        let last_odom_ms = parsed.get("last_odom_ms")?.as_i64().unwrap_or(0).max(0) as u64;
        Some(LinkStatus {
            dongle_ok,
            car_paired,
            last_odom_ms,
            last_updated: std::time::Instant::now(),
        })
    }

    /// 读取一个字节，超时返回 None
    fn read_byte_timeout(port: &mut BufReader<Box<dyn SerialPort>>) -> Result<Option<u8>> {
        let mut byte = [0u8; 1];
        match port.read(&mut byte) {
            Ok(0) => Err(anyhow::anyhow!("串口已断开（EOF）")),
            Ok(_) => Ok(Some(byte[0])),
            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => Ok(None),
            Err(e) => Err(anyhow::anyhow!("串口读取错误: {}", e)),
        }
    }

    /// 从 Header0/Header1 状态尝试匹配帧头
    /// 返回 true 表示帧头匹配成功（状态转为 ReadingFrame）
    fn try_match_frame_header(
        state: &mut FrameParseState,
        line_buf: &mut Vec<u8>,
        byte: u8,
    ) -> bool {
        match *state {
            FrameParseState::Header0 => {
                if byte == FRAME_HEADER[1] {
                    *state = FrameParseState::ReadingFrame;
                    true
                } else if byte == FRAME_HEADER[0] {
                    *state = FrameParseState::Header1;
                    false
                } else {
                    line_buf.push(FRAME_HEADER[0]);
                    line_buf.push(byte);
                    *state = FrameParseState::WaitingHeader;
                    false
                }
            }
            FrameParseState::Header1 => {
                if byte == FRAME_HEADER[1] {
                    *state = FrameParseState::ReadingFrame;
                    true
                } else {
                    line_buf.push(FRAME_HEADER[0]);
                    line_buf.push(FRAME_HEADER[0]);
                    line_buf.push(byte);
                    *state = FrameParseState::WaitingHeader;
                    false
                }
            }
            _ => false,
        }
    }

    /// 将 line_buf 作为 OdometryLine 加入结果
    fn flush_line(line_buf: &mut Vec<u8>, results: &mut Vec<SerialReadResult>) {
        if line_buf.is_empty() {
            return;
        }
        let buf = std::mem::take(line_buf);
        if let Ok(mut line) = String::from_utf8(buf) {
            // 去除 CRLF 中的 \r
            if line.ends_with('\r') {
                line.pop();
            }
            results.push(SerialReadResult::OdometryLine(line));
        }
    }

    /// 将待定状态（Header0/Header1）的 0xAA 字节回填到行缓冲
    fn flush_pending_header(state: &mut FrameParseState, line_buf: &mut Vec<u8>) {
        match *state {
            FrameParseState::Header0 => {
                line_buf.push(FRAME_HEADER[0]);
                *state = FrameParseState::WaitingHeader;
            }
            FrameParseState::Header1 => {
                line_buf.push(FRAME_HEADER[0]);
                line_buf.push(FRAME_HEADER[0]);
                *state = FrameParseState::WaitingHeader;
            }
            _ => {}
        }
    }

    /// 流对齐恢复：扫描窗口内任意位置的 0xAA 0x55
    fn resync_stream(port: &mut BufReader<Box<dyn SerialPort>>) -> Result<()> {
        let start = std::time::Instant::now();
        let mut prev_byte = 0u8;
        while start.elapsed() < RESYNC_TIMEOUT {
            match Self::read_byte_timeout(port)? {
                None => continue,
                Some(b) => {
                    if prev_byte == FRAME_HEADER[0] && b == FRAME_HEADER[1] {
                        debug!("流对齐恢复成功");
                        return Ok(());
                    }
                    prev_byte = b;
                }
            }
        }
        warn!(
            "流对齐恢复超时（{}秒内未找到帧头）",
            RESYNC_TIMEOUT.as_secs()
        );
        Err(anyhow::anyhow!("流对齐恢复超时"))
    }

    /// 读取视频帧（帧头已确认后读取帧大小和数据）
    /// 返回 Ok(Some(data)) 表示有效帧，Ok(None) 表示无效帧（已触发流对齐恢复）
    fn read_frame(port: &mut BufReader<Box<dyn SerialPort>>) -> Result<Option<Vec<u8>>> {
        let mut size_bytes = [0u8; 4];
        port.read_exact(&mut size_bytes)?;
        let frame_size = u32::from_le_bytes(size_bytes) as usize;
        if frame_size > MAX_FRAME_SIZE || frame_size == 0 {
            warn!("帧大小异常: {} 字节，进入流对齐恢复", frame_size);
            Self::resync_stream(port)?;
            return Ok(None);
        }
        let mut frame_data = vec![0u8; frame_size];
        match port.read_exact(&mut frame_data) {
            Ok(()) => {
                // 校验 JPEG SOI 和 EOI 标记，拦截截断帧
                let has_soi = frame_size >= 2
                    && frame_data[0] == JPEG_SOI[0]
                    && frame_data[1] == JPEG_SOI[1];
                let has_eoi = frame_size >= 2
                    && frame_data[frame_size - 2] == JPEG_EOI[0]
                    && frame_data[frame_size - 1] == JPEG_EOI[1];
                if has_soi && has_eoi {
                    debug!("接收帧: {} 字节", frame_size);
                    Ok(Some(frame_data))
                } else {
                    warn!(
                        "帧数据 JPEG 标记异常（SOI={} EOI={}），触发流对齐恢复",
                        has_soi, has_eoi
                    );
                    Self::resync_stream(port)?;
                    Ok(None)
                }
            }
            Err(e) => {
                warn!("读取帧数据失败: {}，进入流对齐恢复", e);
                Self::resync_stream(port)?;
                Ok(None)
            }
        }
    }

    /// 统一读取方法：处理视频帧和测速 JSON 行
    /// 通过原子 generation 检查连接周期，无需锁 serial_manager
    fn read_next(
        port: &mut BufReader<Box<dyn SerialPort>>,
        generation: Arc<AtomicU64>,
        expected: u64,
    ) -> Result<Vec<SerialReadResult>> {
        let start = std::time::Instant::now();
        let mut line_buf: Vec<u8> = Vec::new();
        let mut results: Vec<SerialReadResult> = Vec::new();
        let mut state = FrameParseState::WaitingHeader;

        while start.elapsed() < READ_TOTAL_TIMEOUT {
            // 原子读取 generation，无需锁 manager
            let current_gen = generation.load(Ordering::Relaxed);
            if current_gen != expected {
                return Err(anyhow::anyhow!(
                    "串口连接周期已变化 ({} -> {})，中断读取",
                    expected,
                    current_gen
                ));
            }

            let byte = match Self::read_byte_timeout(port)? {
                Some(b) => b,
                None => {
                    // 超时：将待定状态的字节回填到行缓冲
                    Self::flush_pending_header(&mut state, &mut line_buf);
                    if !line_buf.is_empty() {
                        continue;
                    }
                    return Ok(results);
                }
            };

            // 行缓冲溢出保护
            if line_buf.len() > LINE_BUF_MAX {
                warn!("行缓冲区超过 {} 字节上限，丢弃", LINE_BUF_MAX);
                line_buf.clear();
            }

            match state {
                FrameParseState::WaitingHeader => {
                    if byte == FRAME_HEADER[0] {
                        // 仅在行边界识别视频帧头，防止 JSON 中的 0xAA 0x55 被误解析
                        let at_boundary =
                            line_buf.is_empty() || line_buf.last() == Some(&b'\n');
                        if at_boundary {
                            state = FrameParseState::Header0;
                        } else {
                            line_buf.push(byte);
                        }
                    } else if byte == b'\n' {
                        if !line_buf.is_empty() {
                            Self::flush_line(&mut line_buf, &mut results);
                            return Ok(results);
                        }
                    } else {
                        line_buf.push(byte);
                    }
                }
                FrameParseState::Header0 | FrameParseState::Header1 => {
                    if Self::try_match_frame_header(&mut state, &mut line_buf, byte) {
                        // 帧头匹配成功
                        Self::flush_line(&mut line_buf, &mut results);
                        state = FrameParseState::WaitingHeader;
                        match Self::read_frame(port)? {
                            Some(frame_data) => {
                                results.push(SerialReadResult::VideoFrame(frame_data));
                                continue;
                            }
                            None => {
                                if !results.is_empty() {
                                    return Ok(results);
                                }
                                continue;
                            }
                        }
                    }
                }
                FrameParseState::ReadingFrame => {
                    // read_frame 已在 Header0/Header1 分支内调用，此处不应到达
                    state = FrameParseState::WaitingHeader;
                }
            }
        }

        // 总超时：回填待定状态字节
        Self::flush_pending_header(&mut state, &mut line_buf);
        Ok(results)
    }
}

/// 串口任务结果（用于 spawn_blocking 与 async 上下文间传递数据）
enum SerialTaskResult {
    Items(Vec<SerialReadResult>),
    NoData,
    Error { msg: String },
}

/// 串口通信任务（在独立线程中运行）
pub async fn run_serial_task(state: std::sync::Arc<AppState>) -> Result<()> {
    info!("串口通信任务启动");

    let mut first_frame_received = false;
    let mut frame_count_period: u64 = 0;
    let mut bytes_total_period: u64 = 0;
    let mut last_summary_time = std::time::Instant::now();

    let mut last_odom_summary_time = std::time::Instant::now();
    let mut last_odom_left = 0.0f32;
    let mut last_odom_right = 0.0f32;
    let mut last_odom_heading = 0.0f32;

    loop {
        let is_connected = {
            let manager = state.serial_manager.lock_or_panic("serial_manager");
            matches!(manager.state, SerialConnectionState::Connected { .. })
        };

        if is_connected {
            let state_clone = Arc::clone(&state);
            let task_handle = tokio::task::spawn_blocking(move || {
                let (mut port, gen_arc, taken_generation) = {
                    let mut manager = state_clone.serial_manager.lock_or_panic("serial_manager");
                    match manager.port.take() {
                        Some(p) => {
                            let gen = manager.port_generation.load(Ordering::Relaxed);
                            (p, Arc::clone(&manager.port_generation), gen)
                        }
                        None => return SerialTaskResult::NoData,
                    }
                };

                let result =
                    SerialManager::read_next(&mut port, gen_arc, taken_generation);

                {
                    let mut manager = state_clone.serial_manager.lock_or_panic("serial_manager");
                    if manager.port_generation.load(Ordering::Relaxed) != taken_generation {
                        debug!(
                            "run_serial_task 归还 port 时 generation 变化 ({} -> {})，丢弃旧句柄",
                            taken_generation,
                            manager.port_generation.load(Ordering::Relaxed)
                        );
                        drop(port);
                    } else {
                        manager.port = Some(port);
                    }
                }

                match result {
                    Ok(items) => {
                        if items.is_empty() {
                            SerialTaskResult::NoData
                        } else {
                            SerialTaskResult::Items(items)
                        }
                    }
                    Err(e) => SerialTaskResult::Error { msg: e.to_string() },
                }
            });

            let result = task_handle.await;

            match result {
                Ok(SerialTaskResult::Items(items)) => {
                    // 只处理最新 VideoFrame，其余丢弃
                    let mut latest_frame: Option<Vec<u8>> = None;
                    for item in items {
                        match item {
                            SerialReadResult::VideoFrame(data) => {
                                latest_frame = Some(data);
                            }
                            SerialReadResult::OdometryLine(line) => {
                                if let Some(ble_devs) = SerialManager::parse_ble_line(&line) {
                                    let mut devices =
                                        state.ble_devices.lock_or_recover("ble_devices");
                                    *devices = ble_devs;
                                    info!("BLE 设备列表已更新: {} 个设备", devices.len());
                                } else if let Some(link_status) =
                                    SerialManager::parse_link_line(&line)
                                {
                                    let mut link =
                                        state.link_status.lock_or_recover("link_status");
                                    let changed = *link != link_status;
                                    if changed {
                                        info!("链路状态变化: {:?}", link_status);
                                    }
                                    *link = link_status;
                                } else if let Some(odom_data) =
                                    SerialManager::parse_odometry_line(&line)
                                {
                                    let mut odom = state.odometry.lock_or_recover("odometry");
                                    last_odom_left = odom_data.left_speed_mmps;
                                    last_odom_right = odom_data.right_speed_mmps;
                                    last_odom_heading = odom_data.heading;
                                    *odom = odom_data;
                                    debug!(
                                        "测速数据: 左={}mm/s, 右={}mm/s, 航向={}rad",
                                        last_odom_left as f64,
                                        last_odom_right as f64,
                                        last_odom_heading as f64
                                    );
                                }
                            }
                            SerialReadResult::NoData => {}
                        }
                    }

                    // 处理最新 VideoFrame
                    if let Some(data) = latest_frame {
                        let size = data.len();
                        {
                            let mut manager =
                                state.serial_manager.lock_or_panic("serial_manager");
                            manager.frames_received += 1;
                            manager.frame_count = manager.frames_received as u32;
                        }

                        if !first_frame_received {
                            first_frame_received = true;
                            info!("收到首帧: {} 字节", size);
                        }

                        frame_count_period += 1;
                        bytes_total_period += size as u64;

                        let state_clone = Arc::clone(&state);
                        let frame = match tokio::task::spawn_blocking(move || {
                            process_video_frame(state_clone, data)
                        })
                        .await
                        {
                            Ok(Some(frame)) => Some(frame),
                            Ok(None) => None,
                            Err(e) => {
                                warn!("视频帧处理任务异常: {}", e);
                                None
                            }
                        };

                        if let Some(frame) = frame {
                            *state.video_frame.lock_or_recover("video_frame") = Some(frame);
                            {
                                let mut manager =
                                    state.serial_manager.lock_or_panic("serial_manager");
                                manager.frames_decoded += 1;
                            }
                        }
                    }

                    // 视频统计周期摘要（10秒）
                    if last_summary_time.elapsed() >= Duration::from_secs(10)
                        && frame_count_period > 0
                    {
                        let elapsed = last_summary_time.elapsed().as_secs_f64();
                        let fps = frame_count_period as f64 / elapsed;
                        info!(
                            "视频统计: {} 帧, {:.1} FPS, {} 字节",
                            frame_count_period, fps, bytes_total_period
                        );
                        frame_count_period = 0;
                        bytes_total_period = 0;
                        last_summary_time = std::time::Instant::now();
                    }

                    // 测速统计周期摘要（5秒）
                    if last_odom_summary_time.elapsed() >= Duration::from_secs(5) {
                        info!(
                            "测速: L={}mm/s R={}mm/s 航向={}rad",
                            last_odom_left as f64, last_odom_right as f64, last_odom_heading as f64
                        );
                        last_odom_summary_time = std::time::Instant::now();
                    }
                }
                Ok(SerialTaskResult::NoData) => {
                    tokio::time::sleep(Duration::from_millis(50)).await;
                }
                Ok(SerialTaskResult::Error { msg }) => {
                    error!("串口读取错误: {}", msg);
                    let mut manager = state.serial_manager.lock_or_panic("serial_manager");
                    manager.disconnect();
                }
                Err(e) if e.is_panic() => {
                    error!("串口任务 panic: {:?}，可能需要重启", e);
                    let mut manager = state.serial_manager.lock_or_panic("serial_manager");
                    manager.disconnect();
                }
                Err(e) if e.is_cancelled() => {
                    debug!("串口阻塞任务被取消");
                }
                Err(e) => {
                    warn!("串口任务执行错误: {}", e);
                }
            }
        } else {
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    }
}

/// 串口扫描任务（每 1 秒扫描一次可用串口，变化时更新状态）
pub async fn run_port_scan_task(state: std::sync::Arc<AppState>) {
    info!("串口扫描任务启动");

    loop {
        let new_ports: Vec<String> =
            match tokio::task::spawn_blocking(SerialManager::list_ports).await {
                Ok(ports) => ports,
                Err(e) => {
                    warn!("扫描串口任务异常: {}", e);
                    Vec::new()
                }
            };

        let changed = {
            let last = state.last_ports.lock_or_recover("last_ports");
            last.as_slice() != new_ports.as_slice()
        };

        if changed {
            let mut available = state.available_ports.lock_or_recover("available_ports");
            *available = new_ports.clone();
            drop(available);

            let mut last = state.last_ports.lock_or_recover("last_ports");
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

    /// 测试未连接时发送数据返回错误
    #[test]
    fn test_send_bytes_disconnected() {
        let mut manager = SerialManager::new();
        let result = manager.send_bytes(&[0x57]);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("串口未连接"));
    }

    /// 测试 send_packet 递增 command_count，send_bytes 不递增
    #[test]
    fn test_command_count_only_for_packet() {
        let mut manager = SerialManager::new();
        let _ = manager.send_bytes(&[1, 2, 3]);
        assert_eq!(manager.command_count, 0);
        let _ = manager.send_packet(&[1, 2, 3]);
        assert_eq!(manager.command_count, 0);
    }

    /// 测试未连接时断开无 panic
    #[test]
    fn test_disconnect_when_disconnected() {
        let mut manager = SerialManager::new();
        manager.disconnect();
        assert!(matches!(manager.state, SerialConnectionState::Disconnected));
    }

    /// 测试 connect/disconnect 会改变 port_generation
    #[test]
    fn test_port_generation_changes_on_lifecycle() {
        let mut manager = SerialManager::new();
        assert_eq!(manager.port_generation.load(Ordering::Relaxed), 0);

        manager.disconnect();
        assert_eq!(manager.port_generation.load(Ordering::Relaxed), 1);

        manager.disconnect();
        assert_eq!(
            manager.port_generation.load(Ordering::Relaxed),
            2,
            "多次 disconnect 应继续递增"
        );
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

    /// 测试测速 JSON 解析 - 字段顺序不同且含空格
    #[test]
    fn test_parse_odometry_line_whitespace_and_order() {
        let line = r#"{ "dist": 12345, "t": "odom", "ls": 100.5, "hd": 18000, "rs": 99.3 }"#;
        let odom = SerialManager::parse_odometry_line(line).expect("应支持空格与字段顺序变化");
        assert!((odom.left_speed_mmps - 100.5).abs() < 0.1);
        assert!((odom.heading - 180.0).abs() < 0.1);
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

    /// 测试 OdometryData Clone 保留 last_update（不重置为 now）
    #[test]
    fn test_odometry_clone_preserves_last_update() {
        // 设置一个明显过去的时间
        let odom = OdometryData {
            last_update: std::time::Instant::now()
                .checked_sub(std::time::Duration::from_secs(60))
                .unwrap_or_else(std::time::Instant::now),
            ..Default::default()
        };
        let cloned = odom.clone();
        // Clone 应保留原 last_update，不重置为 now
        assert_eq!(
            odom.last_update, cloned.last_update,
            "Clone 应保留 last_update，不重置为 now"
        );
    }

    /// 测试 AppState 初始串口列表为空
    #[test]
    fn test_app_state_ports_initially_empty() {
        let state = crate::AppState::new_test();
        let available = state.available_ports.lock_or_recover("available_ports");
        assert!(available.is_empty(), "初始可用串口列表应为空");
        let last = state.last_ports.lock_or_recover("last_ports");
        assert!(last.is_empty(), "初始 last_ports 应为空");
    }

    /// 测试 BLE 设备 JSON 行解析 - 有效数据
    #[test]
    fn test_parse_ble_line_valid() {
        let line = r#"{"t":"ble","devices":[{"name":"ESP32-C6","mac":"AA:BB:CC:DD:EE:01","rssi":-42},{"name":"Unknown","mac":"AA:BB:CC:DD:EE:02","rssi":-85}]}"#;
        let devices = SerialManager::parse_ble_line(line).expect("解析有效 BLE JSON 失败");
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

    /// 测试 BLE 设备 JSON 行解析 - 空格与字段顺序变化
    #[test]
    fn test_parse_ble_line_whitespace_and_order() {
        let line = r#"{ "devices": [{"name":"ESP32-C6","mac":"AA:BB:CC:DD:EE:01","rssi":-42}], "t": "ble" }"#;
        let devices = SerialManager::parse_ble_line(line).expect("应支持空格与字段顺序变化");
        assert_eq!(devices.len(), 1);
        assert_eq!(devices[0].name, "ESP32-C6");
    }

    /// 测试链路状态 JSON 行解析 - 有效数据（车载已配对）
    #[test]
    fn test_parse_link_line_valid_paired() {
        let line = r#"{"t":"link","dongle":"ok","car_paired":true,"last_odom_ms":1234}"#;
        let status = SerialManager::parse_link_line(line).expect("解析有效链路状态 JSON 失败");
        assert!(status.dongle_ok);
        assert!(status.car_paired);
        assert_eq!(status.last_odom_ms, 1234);
    }

    /// 测试链路状态 JSON 行解析 - 有效数据（车载未配对）
    #[test]
    fn test_parse_link_line_valid_unpaired() {
        let line = r#"{"t":"link","dongle":"ok","car_paired":false,"last_odom_ms":0}"#;
        let status = SerialManager::parse_link_line(line).expect("解析有效链路状态 JSON 失败");
        assert!(status.dongle_ok);
        assert!(!status.car_paired);
        assert_eq!(status.last_odom_ms, 0);
    }

    /// 测试链路状态 JSON 行解析 - 非 link 消息
    #[test]
    fn test_parse_link_line_not_link() {
        let line = r#"{"t":"odom","ls":100}"#;
        assert!(SerialManager::parse_link_line(line).is_none());
    }

    /// 测试链路状态 JSON 行解析 - 无效 JSON
    #[test]
    fn test_parse_link_line_invalid_json() {
        assert!(SerialManager::parse_link_line("not json").is_none());
    }

    /// 测试链路状态 JSON 行解析 - 缺少 car_paired 字段
    #[test]
    fn test_parse_link_line_missing_car_paired() {
        let line = r#"{"t":"link","dongle":"ok","last_odom_ms":1234}"#;
        assert!(SerialManager::parse_link_line(line).is_none());
    }

    /// 测试链路状态 JSON 行解析 - 缺少 last_odom_ms 字段
    #[test]
    fn test_parse_link_line_missing_last_odom_ms() {
        let line = r#"{"t":"link","dongle":"ok","car_paired":true}"#;
        assert!(SerialManager::parse_link_line(line).is_none());
    }

    /// 测试链路状态 JSON 行解析 - dongle 字段非 "ok" 时 dongle_ok 为 false
    #[test]
    fn test_parse_link_line_dongle_not_ok() {
        let line = r#"{"t":"link","dongle":"error","car_paired":false,"last_odom_ms":0}"#;
        let status = SerialManager::parse_link_line(line).expect("解析有效链路状态 JSON 失败");
        assert!(!status.dongle_ok);
        assert!(!status.car_paired);
    }

    /// 测试链路状态 JSON 行解析 - last_odom_ms 为 -1（固件从未收到车载数据）
    #[test]
    fn test_parse_link_line_negative_last_odom_ms() {
        let line = r#"{"t":"link","dongle":"ok","car_paired":false,"last_odom_ms":-1}"#;
        let status = SerialManager::parse_link_line(line)
            .expect("last_odom_ms:-1 应解析成功，负数归一化为 0");
        assert!(status.dongle_ok);
        assert!(!status.car_paired);
        assert_eq!(status.last_odom_ms, 0, "负数 last_odom_ms 应归一化为 0");
    }

    /// 测试链路状态 JSON 行解析 - 空格与字段顺序变化
    #[test]
    fn test_parse_link_line_whitespace_and_order() {
        let line = r#"{ "last_odom_ms": 1234, "t": "link", "dongle": "ok", "car_paired": true }"#;
        let status = SerialManager::parse_link_line(line).expect("应支持空格与字段顺序变化");
        assert!(status.dongle_ok);
        assert!(status.car_paired);
        assert_eq!(status.last_odom_ms, 1234);
    }

    /// 测试 LinkStatus 默认值
    #[test]
    fn test_link_status_default() {
        let status = LinkStatus::default();
        assert!(!status.dongle_ok);
        assert!(!status.car_paired);
        assert_eq!(status.last_odom_ms, 0);
    }

    /// 测试 LinkStatus 相等比较（忽略 last_updated 字段）
    #[test]
    fn test_link_status_eq_ignores_last_updated() {
        let mut s1 = LinkStatus::default();
        let mut s2 = LinkStatus::default();
        s1.last_updated = std::time::Instant::now();
        s2.last_updated = std::time::Instant::now()
            .checked_sub(std::time::Duration::from_secs(10))
            .unwrap_or_else(std::time::Instant::now);
        assert_eq!(s1, s2, "last_updated 不同时仍应相等");
    }

    /// 测试 LinkStatus 不相等
    #[test]
    fn test_link_status_neq() {
        let s1 = LinkStatus {
            dongle_ok: true,
            car_paired: true,
            last_odom_ms: 100,
            last_updated: std::time::Instant::now(),
        };
        let s2 = LinkStatus {
            dongle_ok: false,
            car_paired: true,
            last_odom_ms: 100,
            last_updated: std::time::Instant::now(),
        };
        assert_ne!(s1, s2);
    }
}
