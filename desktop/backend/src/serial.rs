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
use std::time::Duration;

use anyhow::Result;
use serialport::{SerialPort, SerialPortType};
use tracing::{debug, error, info, warn};

use crate::AppState;

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
    Connected {
        port_name: String,
        baud_rate: u32,
    },
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
}

impl SerialManager {
    /// 创建新管理器
    pub fn new() -> Self {
        Self {
            port: None,
            state: SerialConnectionState::Disconnected,
            frame_count: 0,
            bytes_sent: 0,
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
            debug!("发送命令: 0x{:02X} ('{}')", cmd, cmd as char);
            Ok(())
        } else {
            Err(anyhow::anyhow!("串口未连接"))
        }
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

/// 串口通信任务（在独立线程中运行）
pub async fn run_serial_task(state: std::sync::Arc<AppState>) -> Result<()> {
    info!("串口通信任务启动");
    
    let mut frame_buffer = Vec::new();
    
    loop {
        // 检查串口是否连接
        let is_connected = {
            let manager = state.serial_manager.lock().await;
            matches!(manager.state, SerialConnectionState::Connected { .. })
        };
        
        if is_connected {
            // 读取视频帧
            let mut manager = state.serial_manager.lock().await;
            
            match manager.read_video_frame(&mut frame_buffer) {
                Ok(true) => {
                    // 成功读取帧，更新共享状态
                    let mut video = state.video_frame.lock().await;
                    *video = Some(frame_buffer.clone());
                }
                Ok(false) => {
                    // 未读取到帧
                }
                Err(e) => {
                    error!("串口读取错误: {}", e);
                    manager.disconnect();
                }
            }
        } else {
            // 未连接，等待
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    }
}
