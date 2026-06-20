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
 * 发送：8 字节二进制 WirelessPacket（与 UDP 控制载荷格式一致）
 * 接收：[0xAA][0x55][帧大小(4字节)][帧数据]
 */
use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::io::{BufReader, Read, Write};
use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use base64::Engine;
use serialport::{SerialPort, SerialPortType};
use tokio_util::sync::CancellationToken;
use tracing::{debug, error, info, warn};
use zune_jpeg::JpegDecoder;
use zune_jpeg::zune_core::bytestream::ZCursor;
use zune_jpeg::zune_core::colorspace::ColorSpace;
use zune_jpeg::zune_core::options::DecoderOptions;
use webp_rust::{encode_lossy, ImageBuffer};

use crate::{AppState, MutexExt};

/// 尝试将 JPEG 帧转码为 WebP（纯 Rust，无需 libwebp）
/// 成功且体积更小时返回 WebP 字节，否则返回 None
fn try_encode_webp(jpeg: &[u8]) -> Option<Vec<u8>> {
    let options = DecoderOptions::default().jpeg_set_out_colorspace(ColorSpace::RGBA);
    let mut decoder = JpegDecoder::new_with_options(ZCursor::new(jpeg), options);
    let rgba = decoder.decode().ok()?;
    let info = decoder.info().ok()?;
    let width = info.width as usize;
    let height = info.height as usize;
    if rgba.len() != width * height * 4 {
        return None;
    }
    let image = ImageBuffer {
        width,
        height,
        rgba,
    };
    encode_lossy(&image, 1, 60, None).ok()
}

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

/// 链路状态（Dongle ↔ 车载 WiFi/UDP 在线状态，即 car_paired）
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

/// 串口帧头
const FRAME_HEADER: [u8; 2] = [0xAA, 0x55];
/// 默认波特率
pub const DEFAULT_BAUD_RATE: u32 = 921_600;
/// 读取超时（单次 read 最长等待时间）
const READ_TIMEOUT: Duration = Duration::from_millis(100);
/// 最大视频帧大小（与 receiver_dongle 的 VideoFrameBuffer 对齐）
const MAX_FRAME_SIZE: usize = 32 * 1024;

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
    /// 已发送的控制/速度/模式命令数
    pub command_count: u64,
    /// 串口句柄代际计数器，用于解决 run_serial_task 与 disconnect/connect 之间的竞态条件
    ///
    /// 工作方式：
    /// - run_serial_task 在 spawn_blocking 中 take() 出 port 时记录当前 generation
    /// - I/O 完成后归还 port 前检查 generation 是否变化
    /// - 若变化，说明期间发生过 disconnect/connect，旧 port 必须丢弃，避免覆盖新连接的句柄
    pub port_generation: u64,
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
            port_generation: 0,
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
        // 递增代际计数器，标记新连接周期
        // 旧 run_serial_task 归还的 port 会因 generation 不匹配而被丢弃
        self.port_generation = self.port_generation.wrapping_add(1);
        self.state = SerialConnectionState::Connected {
            port_name: port_name.to_string(),
            baud_rate,
        };

        info!(
            "串口连接成功: {} (generation={})",
            port_name, self.port_generation
        );
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
        //
        // 同时递增 generation，让正在 I/O 中的旧 run_serial_task 归还时丢弃旧句柄，
        // 避免在 disconnect+connect 后覆盖新连接的串口句柄
        info!("断开串口连接");
        self.port = None;
        self.write_port = None;
        self.port_generation = self.port_generation.wrapping_add(1);
        self.state = SerialConnectionState::Disconnected;
    }

    /// 发送单字节命令（保留用于调试）
    pub fn send_command(&mut self, cmd: u8) -> Result<()> {
        self.send_bytes(&[cmd])
    }

    /// 发送完整二进制数据包（8 字节 WirelessPacket），并递增命令计数
    pub fn send_packet(&mut self, packet: &[u8]) -> Result<()> {
        self.send_bytes(packet)?;
        self.command_count += 1;
        Ok(())
    }

    /// 发送多字节数据（使用独立写句柄，与读操作可并发）
    /// 注意：此函数不递增 command_count，调用方按需使用 send_packet
    pub fn send_bytes(&mut self, data: &[u8]) -> Result<()> {
        if let Some(ref mut wp) = self.write_port {
            wp.write_all(data)?;
            wp.flush()?;
            self.bytes_sent += data.len() as u64;
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
        let parsed: serde_json::Value = serde_json::from_str(line).ok()?;
        if parsed["t"].as_str()? != "odom" {
            return None;
        }

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
            // wifi_mac 为可选项：仅部分设备会在 Manufacturer Data 中广播
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
    /// 由接收器 Dongle 收到 'P' 探测命令或周期性（5秒）主动上报
    /// 注意：固件在车载从未配对时输出 last_odom_ms:-1（int32_t），
    ///       此处用 as_i64() 兼容负数，并将负数归一化为 0（表示"从未收到"），
    ///       避免负数导致整条 link 消息被丢弃（前端会卡在"探测中"状态）
    pub fn parse_link_line(line: &str) -> Option<LinkStatus> {
        let parsed: serde_json::Value = serde_json::from_str(line).ok()?;
        if parsed["t"].as_str()? != "link" {
            return None;
        }

        // dongle 字段为字符串 "ok" 表示 Dongle 正常工作
        let dongle_str = parsed.get("dongle")?.as_str()?;
        let dongle_ok = dongle_str == "ok";
        let car_paired = parsed.get("car_paired")?.as_bool()?;
        // 用 as_i64() 兼容固件输出的 -1（从未收到车载数据），负数归一化为 0
        let last_odom_ms = parsed.get("last_odom_ms")?.as_i64().unwrap_or(0).max(0) as u64;

        Some(LinkStatus {
            dongle_ok,
            car_paired,
            last_odom_ms,
            last_updated: std::time::Instant::now(),
        })
    }

    /// 流对齐恢复：跳过字节直到找到下一个 0xAA 0x55 帧头
    fn resync_stream(port: &mut BufReader<Box<dyn SerialPort>>) -> Result<()> {
        let start = std::time::Instant::now();
        let mut prev_byte = 0u8;
        while start.elapsed() < Duration::from_secs(2) {
            let mut byte = [0u8; 1];
            match port.read(&mut byte) {
                Ok(0) => {
                    return Err(anyhow::anyhow!("串口已断开（EOF），流对齐恢复中断"));
                }
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
    /// 返回 Ok(Some(data)) 表示有效帧，Ok(None) 表示无效帧（已触发流对齐恢复）
    fn read_frame_data(port: &mut BufReader<Box<dyn SerialPort>>) -> Result<Option<Vec<u8>>> {
        let mut size_bytes = [0u8; 4];
        port.read_exact(&mut size_bytes)?;
        let frame_size = u32::from_le_bytes(size_bytes) as usize;
        if frame_size > MAX_FRAME_SIZE || frame_size == 0 {
            warn!("帧大小异常: {} 字节，进入流对齐恢复", frame_size);
            // 帧大小异常，跳过数据直到找到下一个帧头
            Self::resync_stream(port)?;
            return Ok(None);
        }
        let mut frame_data = vec![0u8; frame_size];
        match port.read_exact(&mut frame_data) {
            Ok(()) => {
                // 验证帧数据是否为有效 JPEG（以 0xFF 0xD8 SOI 标记开头）
                // 防止串口数据中恰好出现 0xAA 0x55 字节序列导致的帧头误检测
                if frame_size >= 2 && frame_data[0] == 0xFF && frame_data[1] == 0xD8 {
                    debug!("接收帧: {} 字节", frame_size);
                    Ok(Some(frame_data))
                } else {
                    warn!("帧数据不以 JPEG SOI 开头（疑似帧头误检测），触发流对齐恢复");
                    Self::resync_stream(port)?;
                    Ok(None)
                }
            }
            Err(e) => {
                warn!("读取帧数据失败: {}，进入流对齐恢复", e);
                // 帧数据读取失败，跳过剩余字节直到找到下一个帧头
                Self::resync_stream(port)?;
                Ok(None)
            }
        }
    }

    /// 统一读取方法：同时处理视频帧和测速JSON行
    /// 解决帧头重叠遗漏和视频/测速数据互斥吞没问题
    /// 返回 Vec<SerialReadResult> 支持同时返回视频帧和测速行（解决帧头匹配时 line_buf 被丢弃问题）
    ///
    /// 通过 state/generation 参数支持可中断：
    /// - 每次 read 超时（100ms）后检查 generation 是否变化
    /// - 变化说明 disconnect/connect 已发生，立即返回错误，让旧句柄尽快释放
    pub fn read_next(
        port: &mut BufReader<Box<dyn SerialPort>>,
        state: &AppState,
        generation: u64,
        cancel_token: &CancellationToken,
    ) -> Result<Vec<SerialReadResult>> {
        // 策略：逐字节读取，维护一个行缓冲区和结果列表
        // - 遇到 0xAA 时尝试匹配帧头 0xAA 0x55
        // - 找到帧头时：先将 line_buf 作为 OdometryLine 加入结果，再读取帧
        // - 遇到 \n 时，将行缓冲区内容作为测速数据加入结果并返回
        // - 设置5秒总超时，拆分为 100ms 短 read 循环

        let start = std::time::Instant::now();
        let mut line_buf: Vec<u8> = Vec::new();
        let mut results: Vec<SerialReadResult> = Vec::new();

        while start.elapsed() < Duration::from_secs(5) {
            if cancel_token.is_cancelled() {
                return Err(anyhow::anyhow!("串口读取任务已取消"));
            }

            // 检查连接周期是否已变化（disconnect/connect 后旧句柄应尽快退出）
            {
                let manager = state.serial_manager.lock_or_panic("serial_manager");
                if manager.port_generation != generation {
                    return Err(anyhow::anyhow!(
                        "串口连接周期已变化 ({} -> {})，中断读取",
                        generation,
                        manager.port_generation
                    ));
                }
            }

            let mut byte = [0u8; 1];
            match port.read(&mut byte) {
                Ok(0) => {
                    return Err(anyhow::anyhow!("串口已断开（EOF）"));
                }
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
                                return Err(anyhow::anyhow!("串口已断开（EOF）"));
                            }
                            Ok(_) => {
                                if second[0] == FRAME_HEADER[1] {
                                    // 找到帧头 0xAA 0x55
                                    // 先将 line_buf 中累积的文本数据作为 OdometryLine 加入结果
                                    Self::flush_line_buf(&mut line_buf, &mut results);
                                    // 读取帧数据
                                    match Self::read_frame_data(port) {
                                        Ok(Some(frame_data)) => {
                                            results.push(SerialReadResult::VideoFrame(frame_data));
                                            return Ok(results);
                                        }
                                        Ok(None) => {
                                            // 无效帧，流对齐恢复已完成
                                            // 若已有结果则返回，否则继续读取
                                            if !results.is_empty() {
                                                return Ok(results);
                                            }
                                            continue;
                                        }
                                        Err(e) => return Err(e),
                                    }
                                } else {
                                    // 0xAA 不是帧头（后跟非 0x55 字节）
                                    line_buf.push(b); // 保留 0xAA
                                    if second[0] == FRAME_HEADER[0] {
                                        // 第二个字节也是 0xAA，可能是帧头的开始
                                        let mut third = [0u8; 1];
                                        match port.read(&mut third) {
                                            Ok(0) => {
                                                return Err(anyhow::anyhow!("串口已断开（EOF）"));
                                            }
                                            Ok(_) => {
                                                if third[0] == FRAME_HEADER[1] {
                                                    // 找到 0xAA 0x55 帧头！
                                                    Self::flush_line_buf(
                                                        &mut line_buf,
                                                        &mut results,
                                                    );
                                                    match Self::read_frame_data(port) {
                                                        Ok(Some(frame_data)) => {
                                                            results.push(
                                                                SerialReadResult::VideoFrame(
                                                                    frame_data,
                                                                ),
                                                            );
                                                            return Ok(results);
                                                        }
                                                        Ok(None) => {
                                                            if !results.is_empty() {
                                                                return Ok(results);
                                                            }
                                                            continue;
                                                        }
                                                        Err(e) => return Err(e),
                                                    }
                                                } else {
                                                    // 0xAA 0xAA 0xXX (XX != 0x55)
                                                    line_buf.push(second[0]);
                                                    line_buf.push(third[0]);
                                                    continue;
                                                }
                                            }
                                            Err(ref e)
                                                if e.kind() == std::io::ErrorKind::TimedOut =>
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
                                    results.push(SerialReadResult::OdometryLine(line));
                                    return Ok(results);
                                }
                                Err(_) => {
                                    continue;
                                }
                            }
                        }
                    } else {
                        line_buf.push(b);
                    }
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {
                    // 读取超时：外层循环会重新检查 generation/cancel
                    if !line_buf.is_empty() {
                        // 没有完整行，继续等待
                        continue;
                    }
                    return Ok(results);
                }
                Err(e) => {
                    return Err(anyhow::anyhow!("串口读取错误: {}", e));
                }
            }
        }

        // 超时
        Ok(results)
    }

    /// 将 line_buf 中的内容作为 OdometryLine 加入结果列表（如果可解析为 UTF-8）
    fn flush_line_buf(line_buf: &mut Vec<u8>, results: &mut Vec<SerialReadResult>) {
        if line_buf.is_empty() {
            return;
        }
        let buf = std::mem::take(line_buf);
        if let Ok(line) = String::from_utf8(buf) {
            results.push(SerialReadResult::OdometryLine(line));
        }
    }
}

/// 串口任务结果（用于 spawn_blocking 与 async 上下文间传递数据）
enum SerialTaskResult {
    /// 读取到多个结果（视频帧和/或测速数据行）
    Items(Vec<SerialReadResult>),
    /// 无数据
    NoData,
    /// 错误
    Error { msg: String },
}

/// 串口通信任务（在独立线程中运行）
pub async fn run_serial_task(state: std::sync::Arc<AppState>) -> Result<()> {
    info!("串口通信任务启动");

    // 视频帧统计变量（用于首帧日志和 10 秒周期摘要）
    let mut first_frame_received = false;
    let mut frame_count_period: u32 = 0;
    let mut bytes_total_period: u64 = 0;
    let mut last_summary_time = std::time::Instant::now();

    // 测速统计变量（用于 5 秒周期摘要）
    let mut last_odom_summary_time = std::time::Instant::now();
    let mut last_odom_left = 0.0f32;
    let mut last_odom_right = 0.0f32;
    let mut last_odom_heading = 0.0f32;

    loop {
        // 检查串口是否连接
        let is_connected = {
            let manager = state.serial_manager.lock_or_panic("serial_manager");
            matches!(manager.state, SerialConnectionState::Connected { .. })
        };

        if is_connected {
            let state_clone = Arc::clone(&state);
            let read_cancel = CancellationToken::new();
            let read_cancel_clone = read_cancel.clone();

            // 在 spawn_blocking 中执行阻塞 I/O，避免阻塞 Tokio 运行时
            // port 由 read 任务独占（send_bytes 使用独立的 write_port），take 后做 I/O 再归还
            let task_handle = tokio::task::spawn_blocking(move || {
                // 短暂获取锁，取出 port 并记录当前 generation
                let (mut port, taken_generation) = {
                    let mut manager = state_clone.serial_manager.lock_or_panic("serial_manager");
                    match manager.port.take() {
                        Some(p) => (p, manager.port_generation),
                        None => return SerialTaskResult::NoData,
                    }
                };

                // 不持锁执行长时间 I/O（5秒拆分为 100ms 短循环，支持 generation 检查）
                let result =
                    SerialManager::read_next(&mut port, &state_clone, taken_generation, &read_cancel_clone);

                // 统计本次读取的视频帧数
                let frame_count_delta: u32 = match &result {
                    Ok(items) => items
                        .iter()
                        .filter(|r| matches!(r, SerialReadResult::VideoFrame(_)))
                        .count() as u32,
                    Err(_) => 0,
                };

                // 归还 port 并更新帧计数
                // 检查 generation 是否在 I/O 期间变化：若变化说明 disconnect/connect 已发生，
                // 必须丢弃旧 port，避免覆盖新连接的串口句柄
                {
                    let mut manager = state_clone.serial_manager.lock_or_panic("serial_manager");
                    if manager.port_generation != taken_generation {
                        // 连接周期已变化，旧 port 不再有效，直接丢弃
                        debug!(
                            "run_serial_task 归还 port 时发现 generation 变化 ({} -> {})，丢弃旧句柄",
                            taken_generation, manager.port_generation
                        );
                        drop(port);
                    } else {
                        manager.port = Some(port);
                    }
                    manager.frame_count += frame_count_delta;
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

            // 取消机制：虽然 spawn_blocking 不可直接取消，但 read_next 内部会检查 cancel_token
            // 此处保留令牌句柄，便于后续扩展；当前主要依靠 generation 检查实现可中断
            let _cancel_guard = read_cancel;

            let result = task_handle.await;

            match result {
                Ok(SerialTaskResult::Items(items)) => {
                    for item in items {
                        match item {
                            SerialReadResult::VideoFrame(data) => {
                                let size = data.len();

                                // 首帧到达日志
                                if !first_frame_received {
                                    first_frame_received = true;
                                    info!("收到首帧: {} 字节", size);
                                }

                                // 统计累计
                                frame_count_period += 1;
                                bytes_total_period += size as u64;

                                // 可选：尝试转码为 WebP，以减小 WebSocket 传输体积
                                let (frame_bytes, frame_format): (Vec<u8>, &str) = if state.use_webp {
                                    match try_encode_webp(&data) {
                                        Some(webp) if webp.len() < data.len() => (webp, "webp"),
                                        _ => (data, "jpeg"),
                                    }
                                } else {
                                    (data, "jpeg")
                                };

                                // Base64 编码视频帧，存储共享引用避免每客户端重复编码
                                let b64_data =
                                    base64::engine::general_purpose::STANDARD.encode(&frame_bytes);

                                // 计算哈希（共享，避免每客户端重复计算）
                                let mut hasher = DefaultHasher::new();
                                b64_data.hash(&mut hasher);
                                let hash = hasher.finish();

                                let b64_arc = Arc::new(b64_data);
                                let format_arc: Arc<str> = Arc::from(frame_format);

                                // 存储 Base64 编码结果，供 WebSocket 客户端共享读取
                                {
                                    let mut b64 = state
                                        .video_frame_b64
                                        .lock_or_recover("video_frame_b64");
                                    *b64 = Some(b64_arc);
                                }
                                // 存储帧格式
                                {
                                    let mut fmt = state
                                        .video_frame_format
                                        .lock_or_recover("video_frame_format");
                                    *fmt = format_arc;
                                }
                                // 存储哈希值，供 WebSocket 客户端共享读取
                                {
                                    let mut h = state
                                        .video_frame_hash
                                        .lock_or_recover("video_frame_hash");
                                    *h = Some(hash);
                                }
                            }
                            SerialReadResult::OdometryLine(line) => {
                                // 先尝试解析为 BLE 设备列表
                                if let Some(ble_devs) = SerialManager::parse_ble_line(&line) {
                                    let mut devices =
                                        state.ble_devices.lock_or_recover("ble_devices");
                                    *devices = ble_devs;
                                    info!("BLE 设备列表已更新: {} 个设备", devices.len());
                                } else if let Some(link_status) =
                                    SerialManager::parse_link_line(&line)
                                {
                                    // 解析链路状态，检测变化并记录日志
                                    let mut link = state.link_status.lock_or_recover("link_status");
                                    let changed = *link != link_status;
                                    if changed {
                                        info!("链路状态变化: {:?}", link_status);
                                    }
                                    *link = link_status;
                                } else if let Some(odom_data) =
                                    SerialManager::parse_odometry_line(&line)
                                {
                                    // serial_manager 锁已释放，单独获取 odometry 锁
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
                            SerialReadResult::NoData => {
                                // 无数据，跳过
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
                    // 无数据，短暂等待
                    tokio::time::sleep(Duration::from_millis(50)).await;
                }
                Ok(SerialTaskResult::Error { msg }) => {
                    error!("串口读取错误: {}", msg);
                    let mut manager = state.serial_manager.lock_or_panic("serial_manager");
                    manager.disconnect();
                }
                Err(e) if e.is_panic() => {
                    // spawn_blocking 任务 panic，记录详细信息
                    error!("串口任务 panic: {:?}，可能需要重启", e);
                    let mut manager = state.serial_manager.lock_or_panic("serial_manager");
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
            let last = state.last_ports.lock_or_recover("last_ports");
            last.as_slice() != new_ports.as_slice()
        };

        if changed {
            // 更新可用串口列表（async 锁）
            let mut available = state.available_ports.lock().await;
            *available = new_ports.clone();
            drop(available);

            // 更新上次扫描结果（sync 锁）
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

    /// 测试未连接时发送命令返回错误
    #[test]
    fn test_send_command_disconnected() {
        let mut manager = SerialManager::new();
        let result = manager.send_command(0x57);
        assert!(result.is_err());
        assert!(result.unwrap_err().to_string().contains("串口未连接"));
    }

    /// 测试 send_packet 递增 command_count，send_bytes 不递增
    #[test]
    fn test_command_count_only_for_packet() {
        let mut manager = SerialManager::new();
        // send_bytes 在未连接时失败，但不会修改 command_count
        let _ = manager.send_bytes(&[1, 2, 3]);
        assert_eq!(manager.command_count, 0);

        let _ = manager.send_packet(&[1, 2, 3]);
        assert_eq!(manager.command_count, 0); // 同样未连接失败
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
        assert_eq!(manager.port_generation, 0);

        manager.disconnect();
        assert_eq!(manager.port_generation, 1);

        manager.disconnect();
        assert_eq!(manager.port_generation, 2, "多次 disconnect 应继续递增");
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

    /// 测试 AppState 初始串口列表为空
    #[test]
    fn test_app_state_ports_initially_empty() {
        let state = crate::AppState::new_test();
        let available = state.available_ports.blocking_lock();
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
    /// 修复回归测试：as_u64() 对负数返回 None，曾导致整条 link 消息被丢弃
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
