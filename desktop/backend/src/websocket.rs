/**
 * WebSocket通信模块
 * 基于 Axum WebSocket，实现实时双向通信
 *
 * 功能：
 * 1. 处理客户端连接
 * 2. 广播视频帧
 * 3. 接收控制命令并转发到串口
 * 4. 管理连接状态
 *
 * 消息格式：
 * 发送：{"type": "video", "data": "base64..."}
 * 接收：{"type": "command", "data": "W"}
 */
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::Instant;

use axum::{
    extract::ws::{Message, WebSocket, WebSocketUpgrade},
    extract::State,
    response::IntoResponse,
};
use futures::{sink::SinkExt, stream::StreamExt};
use tokio::sync::mpsc;
use tokio_util::sync::CancellationToken;
use tracing::{debug, error, info, warn};

use crate::serial::SerialConnectionState;
use crate::{AppState, MutexExt};

/// 客户端连接
#[derive(Debug)]
struct ClientConnection {
    id: u64,
}

/// WebSocket管理器
pub struct WebSocketManager {
    /// 已连接客户端
    clients: Vec<ClientConnection>,
    /// 下一个客户端ID
    next_id: u64,
}

impl Default for WebSocketManager {
    fn default() -> Self {
        Self::new()
    }
}

impl WebSocketManager {
    pub fn new() -> Self {
        Self {
            clients: Vec::new(),
            next_id: 1,
        }
    }

    pub fn add_client(&mut self) -> u64 {
        let id = self.next_id;
        self.next_id += 1;

        self.clients.push(ClientConnection { id });

        info!(
            "WebSocket客户端连接: #{} (总计: {})",
            id,
            self.clients.len()
        );
        id
    }

    pub fn remove_client(&mut self, id: u64) {
        self.clients.retain(|c| c.id != id);
        info!(
            "WebSocket客户端断开: #{} (剩余: {})",
            id,
            self.clients.len()
        );
    }

    pub fn client_count(&self) -> usize {
        self.clients.len()
    }
}

/// WebSocket处理器
pub async fn ws_handler(
    ws: WebSocketUpgrade,
    State(state): State<Arc<AppState>>,
) -> impl IntoResponse {
    ws.on_upgrade(move |socket| handle_socket(socket, state))
}

/// 处理单个WebSocket连接
async fn handle_socket(socket: WebSocket, state: Arc<AppState>) {
    // 注册客户端
    let client_id = {
        let mut manager = state.ws_manager.lock_or_recover("ws_manager");
        manager.add_client()
    };

    // 拆分 WebSocket 为发送/接收
    let (mut ws_sender, mut ws_receiver) = socket.split();

    // 创建 mpsc 通道用于发送消息（tx 可 clone）
    let (tx, mut rx) = mpsc::channel::<Message>(32);

    // 转发任务：从 mpsc 通道接收消息并发送到 WebSocket
    let forward_task = tokio::spawn(async move {
        while let Some(msg) = rx.recv().await {
            if let Err(e) = ws_sender.send(msg).await {
                error!("WebSocket 转发失败: {}", e);
                break;
            }
        }
        debug!("WebSocket 转发任务正常退出");
    });

    // 发送欢迎消息
    let welcome = serde_json::json!({
        "type": "connected",
        "client_id": client_id,
        "message": "已连接到智能车控制系统"
    });

    if let Err(e) = tx.send(Message::Text(welcome.to_string().into())).await {
        error!("发送欢迎消息失败: {}", e);
        // 关闭转发通道，优雅停止 forward_task
        drop(tx);
        let _ = forward_task.await;
        {
            let mut manager = state.ws_manager.lock_or_recover("ws_manager");
            manager.remove_client(client_id);
        }
        return;
    }

    // 创建取消令牌，用于优雅关闭视频广播任务
    let cancel_token = CancellationToken::new();

    // 初始化心跳时间戳：连接建立时即视为收到一次心跳
    // 防止视频任务在刚连接时立即判定超时
    {
        let mut last_hb = state.last_heartbeat.lock_or_recover("last_heartbeat");
        *last_hb = Instant::now();
    }

    // 视频任务：通过 mpsc tx 发送视频帧、测速数据、串口列表
    let video_tx = tx.clone();
    let video_state = state.clone();
    let video_cancel = cancel_token.clone();
    let video_task = tokio::spawn(async move {
        let mut last_frame_hash: Option<u64> = None;
        let mut last_odometry_send = Instant::now();
        let mut last_ports: Vec<String> = Vec::new();
        let mut last_port_check = Instant::now();
        let mut last_ble_send = Instant::now();
        let mut last_status_send = Instant::now();
        let mut last_link_status: Option<crate::serial::LinkStatus> = None;

        loop {
            // 检查取消信号
            if video_cancel.is_cancelled() {
                debug!("视频广播任务收到取消信号，优雅退出");
                break;
            }

            // 心跳超时检测：客户端 90 秒未发送心跳，判定为死连接
            // 心跳间隔为 30 秒，允许 3 次心跳丢失的容错
            {
                let last_hb = video_state.last_heartbeat.lock_or_recover("last_heartbeat");
                if last_hb.elapsed() > std::time::Duration::from_secs(90) {
                    warn!(
                        "客户端 #{} 心跳超时（{}秒），主动断开连接",
                        client_id,
                        last_hb.elapsed().as_secs()
                    );
                    break;
                }
            }

            // 无客户端时跳过帧处理和测速发送，节省资源
            let client_count = {
                let manager = video_state.ws_manager.lock_or_recover("ws_manager");
                manager.client_count()
            };
            if client_count == 0 {
                tokio::select! {
                    _ = tokio::time::sleep(std::time::Duration::from_secs(1)) => {}
                    _ = video_cancel.cancelled() => {
                        debug!("视频广播任务收到取消信号，优雅退出");
                        break;
                    }
                }
                continue;
            }

            // 检查串口列表变化，变化时广播 port_list（每秒最多检查一次）
            if last_port_check.elapsed() >= std::time::Duration::from_secs(1) {
                last_port_check = Instant::now();
                let current_ports = {
                    let ports = video_state.available_ports.lock().await;
                    ports.clone()
                };
                if current_ports != last_ports {
                    last_ports = current_ports.clone();
                    let port_message = serde_json::json!({
                        "type": "port_list",
                        "ports": current_ports,
                        "timestamp": std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_millis() as i64
                    });
                    match video_tx.try_send(Message::Text(port_message.to_string().into())) {
                        Ok(()) => {}
                        Err(mpsc::error::TrySendError::Full(_)) => {
                            // channel 已满（客户端处理慢），丢弃当前串口列表消息
                            video_state.warn_throttled(
                                "ws_port_list_send_full",
                                "串口列表 channel 已满，丢弃一条消息".to_string(),
                            );
                        }
                        Err(mpsc::error::TrySendError::Closed(_)) => {
                            // 客户端已断开，退出循环
                            break;
                        }
                    }
                }
            }

            // 获取视频帧和预计算的哈希值（共享，避免每客户端重复计算）
            let (frame_b64, frame_hash): (Option<Arc<String>>, Option<u64>) = {
                let b64 = video_state
                    .video_frame_b64
                    .lock_or_recover("video_frame_b64");
                let h = video_state
                    .video_frame_hash
                    .lock_or_recover("video_frame_hash");
                (b64.clone(), *h)
            };

            if let (Some(b64_data), Some(hash)) = (frame_b64.as_ref(), frame_hash) {
                if last_frame_hash != Some(hash) {
                    last_frame_hash = Some(hash);

                    let message = serde_json::json!({
                        "type": "video",
                        "format": "jpeg",
                        "data": b64_data.as_str(),
                        "timestamp": std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_millis() as i64
                    });

                    match video_tx.try_send(Message::Text(message.to_string().into())) {
                        Ok(()) => {}
                        Err(mpsc::error::TrySendError::Full(_)) => {
                            // channel 已满（客户端处理慢），丢弃当前视频帧（视频流可容忍丢帧）
                            video_state.warn_throttled(
                                "ws_video_send_full",
                                "视频 channel 已满，丢弃一帧".to_string(),
                            );
                        }
                        Err(mpsc::error::TrySendError::Closed(_)) => {
                            // 客户端已断开，退出循环
                            break;
                        }
                    }
                }
            }

            // 发送测速数据（固件 200ms 上报一次，此处限流避免冗余发送）
            if last_odometry_send.elapsed() >= std::time::Duration::from_millis(200) {
                last_odometry_send = Instant::now();
                let message = {
                    let odom = video_state.odometry.lock_or_recover("odometry");
                    serde_json::json!({
                        "type": "odometry",
                        "leftSpeed": odom.left_speed_mmps,
                        "rightSpeed": odom.right_speed_mmps,
                        "heading": odom.heading,
                        "distance": odom.total_distance_mm,
                        "timestamp": std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_millis() as i64
                    })
                }; // odom 锁在此处释放

                match video_tx.try_send(Message::Text(message.to_string().into())) {
                    Ok(()) => {}
                    Err(mpsc::error::TrySendError::Full(_)) => {
                        // channel 已满，丢弃当前测速数据
                        video_state.warn_throttled(
                            "ws_odometry_send_full",
                            "测速数据 channel 已满，丢弃一条消息".to_string(),
                        );
                    }
                    Err(mpsc::error::TrySendError::Closed(_)) => {
                        // 客户端已断开，退出循环
                        break;
                    }
                }
            }

            // BLE 设备列表广播（5秒节流，非空时发送）
            if last_ble_send.elapsed() >= std::time::Duration::from_secs(5) {
                let ble_data: Vec<serde_json::Value> = {
                    let devices = video_state.ble_devices.lock_or_recover("ble_devices");
                    if devices.is_empty() {
                        Vec::new()
                    } else {
                        devices
                            .iter()
                            .map(|d| {
                                let mut json = serde_json::json!({
                                    "name": d.name,
                                    "mac": d.mac,
                                    "rssi": d.rssi
                                });
                                // 如果有 WiFi MAC（ESP-NOW 通信用），追加到 JSON 中
                                if let Some(ref wm) = d.wifi_mac {
                                    json["wifi_mac"] = serde_json::Value::String(wm.clone());
                                }
                                json
                            })
                            .collect()
                    }
                }; // devices 锁在此处释放

                if !ble_data.is_empty() {
                    last_ble_send = Instant::now();
                    let ble_message = serde_json::json!({
                        "type": "ble_devices",
                        "devices": ble_data
                    });
                    match video_tx.try_send(Message::Text(ble_message.to_string().into())) {
                        Ok(()) => {}
                        Err(mpsc::error::TrySendError::Full(_)) => {
                            // channel 已满，丢弃当前 BLE 设备列表
                            video_state.warn_throttled(
                                "ws_ble_send_full",
                                "BLE 设备列表 channel 已满，丢弃一条消息".to_string(),
                            );
                        }
                        Err(mpsc::error::TrySendError::Closed(_)) => {
                            // 客户端已断开，退出循环
                            break;
                        }
                    }
                }
            }

            // 链路状态广播（状态变化时推送）
            let current_link_status = {
                let link = video_state.link_status.lock_or_recover("link_status");
                link.clone()
            };
            if last_link_status.as_ref() != Some(&current_link_status) {
                last_link_status = Some(current_link_status.clone());
                let link_message = serde_json::json!({
                    "type": "link_status",
                    "dongle_ok": current_link_status.dongle_ok,
                    "car_paired": current_link_status.car_paired,
                    "last_odom_ms": current_link_status.last_odom_ms
                });
                match video_tx.try_send(Message::Text(link_message.to_string().into())) {
                    Ok(()) => {}
                    Err(mpsc::error::TrySendError::Full(_)) => {
                        // channel 已满，丢弃当前链路状态
                        video_state.warn_throttled(
                            "ws_link_status_send_full",
                            "链路状态 channel 已满，丢弃一条消息".to_string(),
                        );
                    }
                    Err(mpsc::error::TrySendError::Closed(_)) => {
                        // 客户端已断开，退出循环
                        break;
                    }
                }
            }

            // 状态广播（每秒推送，替代前端 /api/status 轮询）
            if last_status_send.elapsed() >= std::time::Duration::from_secs(1) {
                last_status_send = Instant::now();
                let (serial_status, frame_count, command_count) = {
                    let manager = video_state.serial_manager.lock_or_recover("serial_manager");
                    let status_str = match &manager.state {
                        SerialConnectionState::Disconnected => "未连接".to_string(),
                        SerialConnectionState::Connecting => "连接中".to_string(),
                        SerialConnectionState::Connected { port_name, .. } => {
                            format!("已连接:{}", port_name)
                        }
                        SerialConnectionState::Error(msg) => format!("错误:{}", msg),
                    };
                    (status_str, manager.frame_count, manager.command_count)
                };
                let current_speed = video_state.current_speed.load(Ordering::Relaxed);
                let ws_clients = {
                    let ws = video_state.ws_manager.lock_or_recover("ws_manager");
                    ws.client_count()
                };
                let uptime = video_state.started_at.elapsed().as_secs();

                let status_message = serde_json::json!({
                    "type": "status",
                    "serial_status": serial_status,
                    "frame_count": frame_count,
                    "current_speed": current_speed,
                    "ws_clients": ws_clients,
                    "uptime": uptime,
                    "command_count": command_count
                });
                match video_tx.try_send(Message::Text(status_message.to_string().into())) {
                    Ok(()) => {}
                    Err(mpsc::error::TrySendError::Full(_)) => {
                        // channel 已满，丢弃当前状态消息
                        video_state.warn_throttled(
                            "ws_status_send_full",
                            "状态 channel 已满，丢弃一条消息".to_string(),
                        );
                    }
                    Err(mpsc::error::TrySendError::Closed(_)) => {
                        // 客户端已断开，退出循环
                        break;
                    }
                }
            }

            // 使用 select! 等待帧率间隔或取消信号
            tokio::select! {
                _ = tokio::time::sleep(std::time::Duration::from_millis(33)) => {} // ~30 FPS
                _ = video_cancel.cancelled() => {
                    debug!("视频广播任务收到取消信号，优雅退出");
                    break;
                }
            }
        }
    });

    // 处理接收到的消息
    while let Some(result) = ws_receiver.next().await {
        match result {
            Ok(Message::Text(text)) => {
                debug!("收到消息: {}", text);

                if let Err(e) = handle_message(&text, &state).await {
                    warn!("处理消息失败: {}", e);
                }
            }
            Ok(Message::Binary(data)) => {
                debug!("收到二进制数据: {} 字节", data.len());
            }
            Ok(Message::Close(_)) => {
                info!("客户端 #{} 关闭连接", client_id);
                break;
            }
            Ok(Message::Ping(data)) => {
                if let Err(e) = tx.send(Message::Pong(data)).await {
                    warn!("发送Pong失败: {}", e);
                    break;
                }
            }
            Ok(Message::Pong(_)) => {
                // 忽略Pong
            }
            Err(e) => {
                error!("WebSocket错误: {}", e);
                break;
            }
        }
    }

    // 通过 CancellationToken 通知视频任务优雅退出
    cancel_token.cancel();
    // 等待视频任务结束（已收到取消信号，会很快退出）
    if let Err(e) = video_task.await {
        if e.is_panic() {
            error!("视频广播任务 panic: {:?}", e);
        }
    }
    // 关闭转发通道（drop tx 触发 rx 结束）
    drop(tx);
    if let Err(e) = forward_task.await {
        if e.is_panic() {
            error!("WebSocket 转发任务 panic: {:?}", e);
        }
    }

    // 注销客户端
    {
        let mut manager = state.ws_manager.lock_or_recover("ws_manager");
        manager.remove_client(client_id);
    }
}

/// 处理消息
async fn handle_message(text: &str, state: &Arc<AppState>) -> anyhow::Result<()> {
    let message: serde_json::Value = serde_json::from_str(text)?;

    let msg_type = message["type"].as_str().unwrap_or("");
    let data = message["data"].as_str().unwrap_or("");

    match msg_type {
        "command" => {
            // 转发命令到串口
            if let Some(cmd_byte) = data.bytes().next() {
                // 先获取 serial_manager 锁（与 get_status 锁顺序一致：serial_manager → current_speed）
                {
                    let mut manager = state.serial_manager.lock_or_recover("serial_manager");
                    if let Err(e) = manager.send_command(cmd_byte) {
                        warn!("命令发送失败: {}", e);
                        return Err(anyhow::anyhow!("命令发送失败: {}", e));
                    }
                    // 节流式命令转发日志：相同命令 1 秒内只记一次
                    state.log_command_forward(cmd_byte);
                } // 显式释放 serial_manager 锁

                // 如果是速度等级命令(1-9)，同步更新 current_speed
                if (b'1'..=b'9').contains(&cmd_byte) {
                    state
                        .current_speed
                        .store(cmd_byte - b'0', Ordering::Relaxed);
                }
            }
        }
        "speed" => {
            // 速度等级命令（1-9）：同步更新内存状态并通过串口发送
            // 与 command 消息中的 '1'-'9' 行为一致，确保 sendSpeed() API 可用
            if let Ok(speed) = data.parse::<u8>() {
                if !(1..=9).contains(&speed) {
                    warn!("速度值无效: {} (有效范围 1-9)", speed);
                    return Ok(());
                }
                // 向串口发送速度等级字符
                {
                    let mut manager = state.serial_manager.lock_or_recover("serial_manager");
                    // 速度等级字符：1-9 映射为 '1'-'9'
                    if let Err(e) = manager.send_command(b'0' + speed) {
                        warn!("速度命令发送失败: {}", e);
                        return Err(anyhow::anyhow!("速度命令发送失败: {}", e));
                    }
                }
                state.current_speed.store(speed, Ordering::Relaxed);
                info!("设置速度: {}", speed);
            }
        }
        "heartbeat" => {
            // 心跳
            let mut last = state.last_heartbeat.lock_or_recover("last_heartbeat");
            *last = std::time::Instant::now();
        }
        "drive_mode" => {
            // 行走模式切换：原子发送 [T, mode] 双字节，防止中间插入其他命令导致DRIVE_MODE失效
            // 'T' 是 DRIVE_MODE 专属命令字节，与 MAC_CONFIG 的 'M' 不冲突
            if let Some(mode) = message["mode"].as_u64() {
                // 未知模式回退到普通模式（0），防止固件收到无法识别的模式值
                let mode_value = if mode <= 2 { mode as u8 } else { 0u8 };
                {
                    let mut manager = state.serial_manager.lock_or_recover("serial_manager");
                    // 使用 send_bytes 确保 [T, mode] 双字节原子发送
                    if let Err(e) = manager.send_bytes(&[b'T', mode_value]) {
                        warn!("行走模式命令发送失败: {}", e);
                        return Err(anyhow::anyhow!("行走模式命令发送失败: {}", e));
                    }
                }
                info!("切换行走模式: {} (发送值: {})", mode, mode_value);
            }
        }
        "mac_config" => {
            // MAC地址配置：解析MAC字符串并原子转发到串口
            if let Some(mac_str) = message["mac"].as_str() {
                if let Ok(mac_bytes) = parse_mac_address(mac_str) {
                    {
                        let mut manager = state.serial_manager.lock_or_recover("serial_manager");
                        // MAC 配置帧格式：'M' + 0xFF 帧边界标识 + 长度字节(6) + 6字节MAC
                        // 组装为单个 [u8; 9] 数组一次性发送，确保原子性：
                        // 若串口中途断开，接收器不会收到部分 MAC 配置帧
                        let mac_packet: [u8; 9] = [
                            b'M',
                            0xFF,
                            6, // 长度
                            mac_bytes[0],
                            mac_bytes[1],
                            mac_bytes[2],
                            mac_bytes[3],
                            mac_bytes[4],
                            mac_bytes[5],
                        ];
                        if let Err(e) = manager.send_bytes(&mac_packet) {
                            warn!("MAC配置发送失败: {}", e);
                            return Err(anyhow::anyhow!("MAC配置发送失败: {}", e));
                        }
                    }
                    info!("MAC地址配置已转发: {}", mac_str);
                } else {
                    warn!("无效的MAC地址格式: {}", mac_str);
                }
            }
        }
        "ble_scan" => {
            // 触发接收器 BLE 扫描：通过串口发送 'B' 命令
            {
                let mut manager = state.serial_manager.lock_or_recover("serial_manager");
                if let Err(e) = manager.send_command(b'B') {
                    warn!("BLE 扫描命令发送失败: {}", e);
                    return Err(anyhow::anyhow!("BLE 扫描命令发送失败: {}", e));
                }
            }
            info!("已触发 BLE 扫描");
        }
        _ => {
            warn!("未知消息类型: {}", msg_type);
        }
    }

    Ok(())
}

/// 解析MAC地址字符串（格式：AA:BB:CC:DD:EE:FF 或 AABBCCDDEEFF）
fn parse_mac_address(mac_str: &str) -> anyhow::Result<[u8; 6]> {
    let hex_only: String = mac_str.chars().filter(|c| c.is_ascii_hexdigit()).collect();
    if hex_only.len() != 12 {
        return Err(anyhow::anyhow!("MAC地址必须是6字节（12个十六进制字符）"));
    }
    let mut mac = [0u8; 6];
    for i in 0..6 {
        let byte_str = &hex_only[i * 2..i * 2 + 2];
        mac[i] = u8::from_str_radix(byte_str, 16)
            .map_err(|_| anyhow::anyhow!("MAC地址包含无效的十六进制字符"))?;
    }
    Ok(mac)
}

#[cfg(test)]
mod tests {
    use super::*;
    use base64::Engine;

    /// 辅助函数：创建测试用 AppState
    fn create_test_state() -> Arc<AppState> {
        Arc::new(AppState::new())
    }

    /// 测试 WebSocketManager 初始状态
    #[test]
    fn test_ws_manager_new() {
        let manager = WebSocketManager::new();
        assert_eq!(manager.client_count(), 0);
    }

    /// 测试添加客户端递增 ID 和计数
    #[test]
    fn test_add_client() {
        let mut manager = WebSocketManager::new();
        let id1 = manager.add_client();
        let id2 = manager.add_client();
        assert_eq!(id1, 1);
        assert_eq!(id2, 2);
        assert_eq!(manager.client_count(), 2);
    }

    /// 测试移除客户端减少计数
    #[test]
    fn test_remove_client() {
        let mut manager = WebSocketManager::new();
        let id1 = manager.add_client();
        let _id2 = manager.add_client();
        manager.remove_client(id1);
        assert_eq!(manager.client_count(), 1);
    }

    /// 测试移除不存在的客户端无 panic
    #[test]
    fn test_remove_nonexistent_client() {
        let mut manager = WebSocketManager::new();
        manager.add_client();
        manager.remove_client(999);
        assert_eq!(manager.client_count(), 1);
    }

    /// 测试 base64 编码
    #[test]
    fn test_base64_encode() {
        let encoded = base64::engine::general_purpose::STANDARD.encode([0x00, 0x01, 0x02]);
        assert_eq!(encoded, "AAEC");
    }

    /// 测试 handle_message 处理命令消息（无串口连接时返回错误）
    #[tokio::test]
    async fn test_handle_message_command() {
        let state = create_test_state();
        let msg = r#"{"type":"command","data":"W"}"#;

        // 无串口连接时，send_command 会失败，handle_message 应返回错误
        let result = handle_message(msg, &state).await;
        assert!(
            result.is_err(),
            "无串口连接时 handle_message 处理命令消息应返回错误: {:?}",
            result
        );
    }

    /// 测试 handle_message 处理速度等级命令（'1'-'9'），无串口时返回错误
    #[tokio::test]
    async fn test_handle_message_speed_command_updates_state() {
        let state = create_test_state();
        let msg = r#"{"type":"command","data":"7"}"#;

        let result = handle_message(msg, &state).await;
        // 无串口连接时，send_command 失败，handle_message 返回错误
        assert!(
            result.is_err(),
            "无串口连接时处理速度命令应返回错误: {:?}",
            result
        );
    }

    /// 测试 handle_message 处理行走模式切换消息（无串口连接时返回错误）
    #[tokio::test]
    async fn test_handle_message_drive_mode() {
        let state = create_test_state();

        // 测试普通模式
        let msg = r#"{"type":"drive_mode","mode":0}"#;
        let result = handle_message(msg, &state).await;
        assert!(
            result.is_err(),
            "无串口连接时 handle_message 处理行走模式消息应返回错误: {:?}",
            result
        );

        // 测试直线修正模式
        let msg = r#"{"type":"drive_mode","mode":1}"#;
        let result = handle_message(msg, &state).await;
        assert!(
            result.is_err(),
            "无串口连接时 handle_message 处理直线修正模式应返回错误: {:?}",
            result
        );

        // 测试航向锁定模式
        let msg = r#"{"type":"drive_mode","mode":2}"#;
        let result = handle_message(msg, &state).await;
        assert!(
            result.is_err(),
            "无串口连接时 handle_message 处理航向锁定模式应返回错误: {:?}",
            result
        );
    }

    /// 测试 handle_message 处理无效 JSON（应返回解析错误）
    #[tokio::test]
    async fn test_handle_message_invalid_json() {
        let state = create_test_state();
        let msg = "这不是有效的JSON";

        let result = handle_message(msg, &state).await;
        assert!(result.is_err(), "handle_message 处理无效 JSON 时应返回错误");
    }

    /// 测试 handle_message 处理未知消息类型（不 panic，正常返回 Ok）
    #[tokio::test]
    async fn test_handle_message_unknown_type() {
        let state = create_test_state();
        let msg = r#"{"type":"unknown_type","data":"test"}"#;

        let result = handle_message(msg, &state).await;
        assert!(
            result.is_ok(),
            "handle_message 处理未知消息类型时应正常返回: {:?}",
            result
        );
    }

    /// 测试 handle_message 处理心跳消息（验证 last_heartbeat 被更新）
    #[tokio::test]
    async fn test_handle_message_heartbeat() {
        let state = create_test_state();

        // 记录初始心跳时间
        let initial = *state.last_heartbeat.lock_or_recover("last_heartbeat");

        // 短暂等待确保时间差
        tokio::time::sleep(std::time::Duration::from_millis(10)).await;

        let msg = r#"{"type":"heartbeat"}"#;
        let result = handle_message(msg, &state).await;
        assert!(
            result.is_ok(),
            "handle_message 处理心跳消息时不应返回错误: {:?}",
            result
        );

        // 验证心跳时间已更新
        let updated = *state.last_heartbeat.lock_or_recover("last_heartbeat");
        assert!(updated > initial, "心跳时间应在处理后更新");
    }

    /// 测试多客户端并发添加到 WebSocketManager，验证广播逻辑的基础：客户端管理正确性
    #[tokio::test]
    async fn test_multiple_clients_concurrent() {
        let manager = Arc::new(tokio::sync::Mutex::new(WebSocketManager::new()));
        let mut handles = Vec::new();

        // 并发添加 10 个客户端
        for _ in 0..10 {
            let mgr = manager.clone();
            let handle = tokio::spawn(async move {
                let mut m = mgr.lock().await;
                m.add_client()
            });
            handles.push(handle);
        }

        // 等待所有任务完成，收集客户端 ID
        let mut ids = Vec::new();
        for handle in handles {
            let id = handle.await.expect("并发添加客户端任务不应 panic");
            ids.push(id);
        }

        // 验证所有 ID 唯一
        ids.sort();
        ids.dedup();
        assert_eq!(ids.len(), 10, "10 个并发客户端应分配到 10 个唯一 ID");

        // 验证客户端总数正确
        let m = manager.lock().await;
        assert_eq!(m.client_count(), 10, "并发添加后客户端总数应为 10");
    }

    /// 测试并发添加和移除客户端的正确性
    #[tokio::test]
    async fn test_concurrent_add_and_remove() {
        let manager = Arc::new(tokio::sync::Mutex::new(WebSocketManager::new()));

        // 先添加 5 个客户端
        let mut ids = Vec::new();
        for _ in 0..5 {
            let mut m = manager.lock().await;
            ids.push(m.add_client());
        }

        // 并发移除前 3 个客户端
        let mut handles = Vec::new();
        for &id in &ids[..3] {
            let mgr = manager.clone();
            let handle = tokio::spawn(async move {
                let mut m = mgr.lock().await;
                m.remove_client(id);
            });
            handles.push(handle);
        }

        for handle in handles {
            handle.await.expect("并发移除客户端任务不应 panic");
        }

        // 验证剩余客户端数
        let m = manager.lock().await;
        assert_eq!(m.client_count(), 2, "移除 3 个后应剩余 2 个客户端");
    }

    /// 测试 parse_mac_address 解析标准格式 MAC
    #[test]
    fn test_parse_mac_address_standard() {
        let mac = parse_mac_address("AA:BB:CC:DD:EE:FF").expect("标准格式MAC应解析成功");
        assert_eq!(mac, [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF]);
    }

    /// 测试 parse_mac_address 解析无分隔符格式
    #[test]
    fn test_parse_mac_address_no_separator() {
        let mac = parse_mac_address("AABBCCDDEEFF").expect("无分隔符MAC应解析成功");
        assert_eq!(mac, [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF]);
    }

    /// 测试 parse_mac_address 解析小写格式
    #[test]
    fn test_parse_mac_address_lowercase() {
        let mac = parse_mac_address("aa:bb:cc:dd:ee:ff").expect("小写MAC应解析成功");
        assert_eq!(mac, [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF]);
    }

    /// 测试 parse_mac_address 无效长度应失败
    #[test]
    fn test_parse_mac_address_invalid_length() {
        assert!(
            parse_mac_address("AA:BB:CC:DD:EE").is_err(),
            "5字节MAC应解析失败"
        );
        assert!(
            parse_mac_address("AA:BB:CC:DD:EE:FF:00").is_err(),
            "7字节MAC应解析失败"
        );
    }

    /// 测试 parse_mac_address 包含无效字符应失败
    #[test]
    fn test_parse_mac_address_invalid_chars() {
        assert!(
            parse_mac_address("GG:BB:CC:DD:EE:FF").is_err(),
            "包含G的MAC应解析失败"
        );
    }

    /// 测试 handle_message 处理 mac_config 消息（无串口连接时返回错误）
    #[tokio::test]
    async fn test_handle_message_mac_config() {
        let state = create_test_state();
        let msg = r#"{"type":"mac_config","mac":"AA:BB:CC:DD:EE:FF"}"#;

        let result = handle_message(msg, &state).await;
        assert!(
            result.is_err(),
            "无串口连接时 handle_message 处理 mac_config 应返回错误: {:?}",
            result
        );
    }

    /// 测试 handle_message 处理无效 mac_config 格式
    #[tokio::test]
    async fn test_handle_message_mac_config_invalid() {
        let state = create_test_state();
        let msg = r#"{"type":"mac_config","mac":"invalid"}"#;

        let result = handle_message(msg, &state).await;
        assert!(
            result.is_ok(),
            "handle_message 处理无效 mac_config 时不应返回错误: {:?}",
            result
        );
    }

    /// 测试 handle_message 处理 ble_scan 消息（无串口连接时返回错误）
    #[tokio::test]
    async fn test_handle_message_ble_scan() {
        let state = create_test_state();
        let msg = r#"{"type":"ble_scan"}"#;

        let result = handle_message(msg, &state).await;
        assert!(
            result.is_err(),
            "无串口连接时 handle_message 处理 ble_scan 应返回错误: {:?}",
            result
        );
    }
}
