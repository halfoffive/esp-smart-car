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
 * 发送：{"type": "video", "format": "jpeg", "data": "base64...", "timestamp": 123456789}
 * 接收：{"type": "command", "data": "W"}
 */
use std::collections::HashSet;
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use axum::{
    extract::ws::{Message, WebSocket, WebSocketUpgrade},
    extract::{Query, State},
    http::{header, HeaderMap, StatusCode},
    response::IntoResponse,
};
use futures::{sink::SinkExt, stream::StreamExt};
use serde_json::json;
use subtle::ConstantTimeEq;
use tokio::sync::{mpsc, Notify};
use tokio_util::sync::CancellationToken;
use tracing::{debug, error, info, warn};

use crate::serial::{LinkStatus, SerialConnectionState};
use crate::{AppState, MutexExt};

/// WirelessPacket 魔数
const WIRELESS_MAGIC: u8 = 0xA5;
/// WirelessPacket 协议版本
const WIRELESS_VERSION: u8 = 1;

/// 心跳超时时间（秒）：30 秒间隔，允许 3 次丢失
const HEARTBEAT_TIMEOUT_SECS: u64 = 90;
/// 测速数据发送间隔
const ODOMETRY_INTERVAL: Duration = Duration::from_millis(200);
/// BLE 设备列表发送间隔
const BLE_INTERVAL: Duration = Duration::from_secs(5);
/// 状态广播间隔
const STATUS_INTERVAL: Duration = Duration::from_secs(1);
/// 串口列表检查间隔
const PORT_CHECK_INTERVAL: Duration = Duration::from_secs(1);

/// mpsc 通道容量
const MPSC_CAPACITY: usize = 128;
/// mpsc 连续 Full 阈值，超过则断开慢客户端
const MPSC_FULL_DISCONNECT_THRESHOLD: usize = 10;
/// 事件等待回退间隔（替代固定 1ms 轮询）
const EVENT_WAIT_TIMEOUT: Duration = Duration::from_millis(1);

/// WebSocket管理器
pub struct WebSocketManager {
    /// 下一个客户端ID
    next_id: u64,
    /// 活跃客户端ID集合
    client_ids: HashSet<u64>,
    /// 已成功通过 WebSocket 发送的视频帧数
    frames_broadcasted: u64,
}

impl Default for WebSocketManager {
    fn default() -> Self {
        Self::new()
    }
}

impl WebSocketManager {
    pub fn new() -> Self {
        Self {
            next_id: 1,
            client_ids: HashSet::new(),
            frames_broadcasted: 0,
        }
    }

    pub fn add_client(&mut self) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        self.client_ids.insert(id);
        info!("WebSocket客户端连接: #{} (总计: {})", id, self.client_ids.len());
        id
    }

    pub fn remove_client(&mut self, id: u64) {
        if self.client_ids.remove(&id) {
            info!("WebSocket客户端断开: #{} (剩余: {})", id, self.client_ids.len());
        } else {
            warn!("WebSocket客户端断开: #{} 未在活跃集合中", id);
        }
    }

    pub fn client_count(&self) -> usize {
        self.client_ids.len()
    }

    pub fn increment_frames_broadcasted(&mut self) -> u64 {
        self.frames_broadcasted += 1;
        self.frames_broadcasted
    }

    pub fn frames_broadcasted(&self) -> u64 {
        self.frames_broadcasted
    }
}

/// WebSocket 握手查询参数
#[derive(Debug, serde::Deserialize)]
pub struct WsQuery {
    token: Option<String>,
}

/// WebSocket处理器
pub async fn ws_handler(
    ws: WebSocketUpgrade,
    headers: HeaderMap,
    Query(query): Query<WsQuery>,
    State(state): State<Arc<AppState>>,
) -> impl IntoResponse {
    if let Some(ref expected) = state.api_token {
        let provided = headers
            .get(header::AUTHORIZATION)
            .and_then(|h| h.to_str().ok())
            .and_then(|s| s.strip_prefix("Bearer "))
            .or_else(|| query.token.as_deref())
            .unwrap_or("");
        if !constant_time_eq(expected.as_ref().as_bytes(), provided.as_bytes()) {
            warn!("WebSocket 认证失败：token 不匹配");
            let body = json!({"error": "Unauthorized"}).to_string();
            return (
                StatusCode::UNAUTHORIZED,
                [(axum::http::header::CONTENT_TYPE, "application/json")],
                body,
            )
                .into_response();
        }
    }

    ws.on_upgrade(move |socket| handle_socket(socket, state))
}

/// 恒定时间比较，避免时序攻击
fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    a.ct_eq(b).into()
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
    let (tx, mut rx) = mpsc::channel::<Message>(MPSC_CAPACITY);

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
    let welcome = json!({
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

    // 按客户端持有心跳时间戳：连接建立时即视为收到一次心跳
    // 防止视频任务在刚连接时立即判定超时
    let client_heartbeat = Arc::new(Mutex::new(Instant::now()));

    // 事件通知器，用于主循环到视频广播任务的唤醒（替代固定 1ms 轮询）
    let event_notify = Arc::new(Notify::new());

    // 视频任务：通过 mpsc tx 发送视频帧、测速数据、串口列表
    let video_tx = tx.clone();
    let video_state = state.clone();
    let video_cancel = cancel_token.clone();
    let video_heartbeat = Arc::clone(&client_heartbeat);
    let video_notify = Arc::clone(&event_notify);
    let video_task = tokio::spawn(async move {
        video_broadcast_task(
            client_id,
            video_state,
            video_tx,
            video_cancel,
            video_heartbeat,
            video_notify,
        )
        .await
    });

    // 处理接收到的消息
    loop {
        tokio::select! {
            result = ws_receiver.next() => {
                match result {
                    Some(Ok(Message::Text(text))) => {
                        debug!("收到消息: {}", text);

                        if let Err(e) = handle_message(&text, &state, &client_heartbeat, &tx).await {
                            warn!("处理消息失败: {}", e);
                        }
                        event_notify.notify_one();
                    }
                    Some(Ok(Message::Binary(_))) => {
                        send_error(&tx, "不支持二进制消息，请发送 JSON 文本").await;
                        event_notify.notify_one();
                    }
                    Some(Ok(Message::Close(_))) => {
                        info!("客户端 #{} 关闭连接", client_id);
                        break;
                    }
                    Some(Ok(Message::Ping(data))) => {
                        if let Err(e) = tx.send(Message::Pong(data)).await {
                            warn!("发送Pong失败: {}", e);
                            break;
                        }
                        let mut guard = client_heartbeat.lock_or_recover("client_heartbeat");
                        *guard = Instant::now();
                        event_notify.notify_one();
                    }
                    Some(Ok(Message::Pong(_))) => {
                        let mut guard = client_heartbeat.lock_or_recover("client_heartbeat");
                        *guard = Instant::now();
                        event_notify.notify_one();
                    }
                    Some(Err(e)) => {
                        error!("WebSocket错误: {}", e);
                        break;
                    }
                    None => {
                        info!("客户端 #{} 连接已关闭", client_id);
                        break;
                    }
                }
            }
            _ = cancel_token.cancelled() => {
                info!("客户端 #{} 取消信号触发，主循环退出", client_id);
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
    // 优雅通知客户端关闭
    let _ = tx.send(Message::Close(None)).await;
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

/// 视频广播任务：发送视频帧、测速、BLE、链路状态、状态等消息
async fn video_broadcast_task(
    client_id: u64,
    video_state: Arc<AppState>,
    video_tx: mpsc::Sender<Message>,
    video_cancel: CancellationToken,
    video_heartbeat: Arc<Mutex<Instant>>,
    event_notify: Arc<Notify>,
) {
    let mut last_frame_hash: Option<u64> = None;
    let mut last_odometry_send = Instant::now();
    let mut last_ports: Vec<String> = Vec::new();
    let mut last_port_check = Instant::now();
    let mut last_ble_send = Instant::now();
    let mut last_status_send = Instant::now();
    let mut last_link_status: Option<LinkStatus> = None;
    let mut consecutive_full = 0usize;
    let mut pending_ports: Option<Vec<String>> = None;

    loop {
        // 检查取消信号
        if video_cancel.is_cancelled() {
            debug!("视频广播任务收到取消信号，优雅退出");
            break;
        }

        // 心跳超时检测：客户端 90 秒未发送心跳，判定为死连接
        {
            let mut should_disconnect = false;
            let mut lock_busy = false;
            {
                match video_heartbeat.try_lock() {
                    Ok(last_hb) => {
                        if last_hb.elapsed() > Duration::from_secs(HEARTBEAT_TIMEOUT_SECS) {
                            should_disconnect = true;
                        }
                    }
                    Err(_) => {
                        lock_busy = true;
                    }
                }
            }
            if should_disconnect {
                warn!(
                    "客户端 #{} 心跳超时（{}秒），主动断开连接",
                    client_id, HEARTBEAT_TIMEOUT_SECS
                );
                video_cancel.cancel();
                break;
            }
            if lock_busy {
                tokio::task::yield_now().await;
            }
        }

        // 检查串口列表变化，变化时暂存到 pending_ports，随下次 status 一起发送
        if last_port_check.elapsed() >= PORT_CHECK_INTERVAL {
            last_port_check = Instant::now();
            let current_ports = {
                let ports = video_state
                    .available_ports
                    .lock_or_recover("available_ports");
                ports.clone()
            };
            if current_ports != last_ports {
                last_ports = current_ports.clone();
                pending_ports = Some(current_ports);
            }
        }

        // 获取共享视频帧（单锁保证 data/hash 一致）
        let (frame_raw, frame_hash): (Option<Arc<Vec<u8>>>, Option<u64>) = {
            let vf = video_state.video_frame.lock_or_recover("video_frame");
            vf.as_ref()
                .map(|f| (Some(f.data.clone()), Some(f.hash)))
                .unwrap_or((None, None))
        };

        let mut frame_sent = false;

        if let (Some(raw_data), Some(hash)) = (frame_raw.as_ref(), frame_hash) {
            if last_frame_hash != Some(hash) {
                last_frame_hash = Some(hash);

                // 构建二进制视频消息：[frame_hash(8字节 LE)][timestamp(8字节 LE)][JPEG数据]
                let ts = SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_millis() as u64;
                let header_len = 16; // 8B hash + 8B timestamp
                let mut bin_msg = Vec::with_capacity(header_len + raw_data.len());
                bin_msg.extend_from_slice(&hash.to_le_bytes());
                bin_msg.extend_from_slice(&ts.to_le_bytes());
                bin_msg.extend_from_slice(raw_data);

                if !send_ws_message(
                    &video_tx,
                    &video_state,
                    Message::Binary(bin_msg.into()),
                    "ws_video_send_full",
                    &mut consecutive_full,
                    &video_cancel,
                    client_id,
                ) {
                    break;
                }

                frame_sent = true;

                {
                    let mut manager = video_state.ws_manager.lock_or_recover("ws_manager");
                    manager.increment_frames_broadcasted();
                }
            }
        }

        // 发送测速数据（固件 200ms 上报一次，此处限流避免冗余发送）
        if last_odometry_send.elapsed() >= ODOMETRY_INTERVAL {
            last_odometry_send = Instant::now();
            let message = {
                let odom = video_state.odometry.lock_or_recover("odometry");
                json!({
                    "type": "odometry",
                    "leftSpeed": odom.left_speed_mmps,
                    "rightSpeed": odom.right_speed_mmps,
                    "heading": odom.heading,
                    "distance": odom.total_distance_mm,
                    "timestamp": SystemTime::now()
                        .duration_since(UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_millis() as i64
                })
            }; // odom 锁在此处释放

            if !send_ws_message(
                &video_tx,
                &video_state,
                Message::Text(message.to_string().into()),
                "ws_odometry_send_full",
                &mut consecutive_full,
                &video_cancel,
                client_id,
            ) {
                break;
            }
        }

        // BLE 设备列表广播（5 秒节流，空列表也发送以清空前端）
        if last_ble_send.elapsed() >= BLE_INTERVAL {
            last_ble_send = Instant::now();
            let ble_data: Vec<serde_json::Value> = {
                let devices = video_state.ble_devices.lock_or_recover("ble_devices");
                devices
                    .iter()
                    .map(|d| {
                        let mut json = json!({
                            "name": d.name,
                            "mac": d.mac,
                            "rssi": d.rssi
                        });
                        // 如果有 WiFi MAC（固定热点场景用），追加到 JSON 中
                        if let Some(ref wm) = d.wifi_mac {
                            json["wifi_mac"] = serde_json::Value::String(wm.clone());
                        }
                        json
                    })
                    .collect()
            }; // devices 锁在此处释放

            let ble_message = json!({
                "type": "ble_devices",
                "devices": ble_data
            });
            if !send_ws_message(
                &video_tx,
                &video_state,
                Message::Text(ble_message.to_string().into()),
                "ws_ble_send_full",
                &mut consecutive_full,
                &video_cancel,
                client_id,
            ) {
                break;
            }
        }

        // 链路状态广播（状态变化时推送）
        let current_link_status = {
            let link = video_state.link_status.lock_or_recover("link_status");
            link.clone()
        };
        if last_link_status.as_ref() != Some(&current_link_status) {
            last_link_status = Some(current_link_status.clone());
            let link_message = json!({
                "type": "link_status",
                "dongle_ok": current_link_status.dongle_ok,
                "car_paired": current_link_status.car_paired,
                "last_odom_ms": current_link_status.last_odom_ms
            });
            if !send_ws_message(
                &video_tx,
                &video_state,
                Message::Text(link_message.to_string().into()),
                "ws_link_status_send_full",
                &mut consecutive_full,
                &video_cancel,
                client_id,
            ) {
                break;
            }
        }

        // 状态广播（每秒推送，替代前端 /api/status 轮询）
        // 合并 pending_ports 到同一次发送
        let mut status_sent = false;
        if last_status_send.elapsed() >= STATUS_INTERVAL {
            last_status_send = Instant::now();
            status_sent = true;

            let (serial_status, frame_count, command_count, frames_received, frames_decoded) =
                read_serial_status(&video_state).await;
            let frames_broadcasted = {
                let manager = video_state.ws_manager.lock_or_recover("ws_manager");
                manager.frames_broadcasted()
            };
            let current_speed = video_state.current_speed.load(Ordering::Relaxed);
            let current_drive_mode = video_state.current_drive_mode.load(Ordering::Relaxed);
            let ws_clients = {
                let ws = video_state.ws_manager.lock_or_recover("ws_manager");
                ws.client_count()
            };
            let uptime = video_state.started_at.elapsed().as_secs();

            let mut status_message = json!({
                "type": "status",
                "serial_status": serial_status,
                "frame_count": frame_count,
                "frames_received": frames_received,
                "frames_decoded": frames_decoded,
                "frames_broadcasted": frames_broadcasted,
                "current_speed": current_speed,
                "drive_mode": current_drive_mode,
                "ws_clients": ws_clients,
                "uptime": uptime,
                "command_count": command_count
            });

            if let Some(ports) = pending_ports.take() {
                status_message["ports"] = json!(ports);
            }

            if !send_ws_message(
                &video_tx,
                &video_state,
                Message::Text(status_message.to_string().into()),
                "ws_status_send_full",
                &mut consecutive_full,
                &video_cancel,
                client_id,
            ) {
                break;
            }
        }

        // 如果 ports 有变化但状态尚未发送，单独发送 port_list
        if !status_sent {
            if let Some(ports) = pending_ports.take() {
                let port_message = json!({
                    "type": "port_list",
                    "ports": ports,
                    "timestamp": SystemTime::now()
                        .duration_since(UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_millis() as i64
                });
                if !send_ws_message(
                    &video_tx,
                    &video_state,
                    Message::Text(port_message.to_string().into()),
                    "ws_port_list_send_full",
                    &mut consecutive_full,
                    &video_cancel,
                    client_id,
                ) {
                    break;
                }
            }
        }

        // 仅在未发送新帧时等待，确保新帧到达后能立即被发出
        // 使用 Notify + timeout 替代固定 1ms 轮询，主循环消息到达时可立即唤醒
        if !frame_sent {
            tokio::select! {
                _ = event_notify.notified() => {}
                _ = tokio::time::sleep(EVENT_WAIT_TIMEOUT) => {}
                _ = video_cancel.cancelled() => {
                    debug!("视频广播任务收到取消信号，优雅退出");
                    break;
                }
            }
        }
    }
}

/// 尝试通过 mpsc 发送消息，记录连续 Full 次数，必要时断开慢客户端
fn send_ws_message(
    video_tx: &mpsc::Sender<Message>,
    video_state: &Arc<AppState>,
    msg: Message,
    category: &str,
    consecutive_full: &mut usize,
    video_cancel: &CancellationToken,
    client_id: u64,
) -> bool {
    match video_tx.try_send(msg) {
        Ok(()) => {
            *consecutive_full = 0;
            true
        }
        Err(mpsc::error::TrySendError::Full(_)) => {
            *consecutive_full += 1;
            if *consecutive_full >= MPSC_FULL_DISCONNECT_THRESHOLD {
                warn!(
                    "客户端 #{} mpsc 连续 Full 超过阈值，主动断开连接",
                    client_id
                );
                video_cancel.cancel();
                false
            } else {
                video_state.warn_throttled(
                    category,
                    "WebSocket 发送 channel 已满，丢弃一条消息".to_string(),
                );
                true
            }
        }
        Err(mpsc::error::TrySendError::Closed(_)) => {
            // 客户端已断开，触发取消并退出循环
            video_cancel.cancel();
            false
        }
    }
}

/// 在 spawn_blocking 中读取串口状态与计数器
///
/// video_task 不直接锁 SerialManager，所有串口相关操作保留在 spawn_blocking 中。
async fn read_serial_status(video_state: &Arc<AppState>) -> (String, u64, u64, u64, u64) {
    let state_clone = Arc::clone(video_state);
    match tokio::task::spawn_blocking(move || {
        let manager = state_clone.serial_manager.lock_or_panic("serial_manager");
        let status_str = match &manager.state {
            SerialConnectionState::Disconnected => "未连接".to_string(),
            SerialConnectionState::Connecting => "连接中".to_string(),
            SerialConnectionState::Connected { port_name, .. } => {
                format!("已连接:{}", port_name)
            }
            SerialConnectionState::Error(msg) => format!("错误:{}", msg),
        };
        (
            status_str,
            manager.frame_count,
            manager.command_count,
            manager.frames_received,
            manager.frames_decoded,
        )
    })
    .await
    {
        Ok(result) => result,
        Err(e) => {
            error!("读取串口状态任务异常: {}", e);
            ("未知".to_string(), 0, 0, 0, 0)
        }
    }
}

/// 构建 8 字节 WirelessPacket 二进制数据包
///
/// 布局（与 firmware/libraries/wireless_protocol/src/wireless.h 中的
/// `struct __attribute__((packed)) WirelessPacket` 完全一致）：
/// - bytes[0] = WIRELESS_MAGIC (0xA5)
/// - bytes[1] = WIRELESS_VERSION (1)
/// - bytes[2] = type_value (CommandType discriminant)
/// - bytes[3] = data
/// - bytes[4] = speed
/// - bytes[5..7] = seq (little-endian u16)
/// - bytes[7] = checksum（bytes[0..7] 累加和的低 8 位）
pub(crate) fn build_wireless_packet(type_value: u8, data: u8, speed: u8, seq: u16) -> [u8; 8] {
    let mut packet = [0u8; 8];
    packet[0] = WIRELESS_MAGIC;
    packet[1] = WIRELESS_VERSION;
    packet[2] = type_value;
    packet[3] = data;
    packet[4] = speed;
    let seq_bytes = seq.to_le_bytes();
    packet[5] = seq_bytes[0];
    packet[6] = seq_bytes[1];
    let checksum = packet[0..7].iter().fold(0u8, |acc, &b| acc.wrapping_add(b));
    packet[7] = checksum;
    packet
}

/// 通过 WebSocket 发送错误消息给客户端
async fn send_error(tx: &mpsc::Sender<Message>, msg: &str) {
    let error = json!({
        "type": "error",
        "message": msg
    });
    if let Err(e) = tx.send(Message::Text(error.to_string().into())).await {
        debug!("发送错误消息失败（客户端可能已断开）: {}", e);
    }
}

/// 在 spawn_blocking 中执行串口发送，避免阻塞 async 运行时
async fn send_packet_blocking(
    state: &Arc<AppState>,
    packet: [u8; 8],
    tx: &mpsc::Sender<Message>,
    error_msg: &str,
) -> anyhow::Result<()> {
    let state_clone = Arc::clone(state);
    match tokio::task::spawn_blocking(move || {
        let mut manager = state_clone.serial_manager.lock_or_panic("serial_manager");
        manager.send_packet(&packet)
    })
    .await
    {
        Ok(Ok(())) => Ok(()),
        Ok(Err(e)) => {
            let msg = format!("{}: {}", error_msg, e);
            send_error(tx, &msg).await;
            Err(anyhow::anyhow!(msg))
        }
        Err(e) => {
            let msg = format!("发送任务异常: {}", e);
            send_error(tx, &msg).await;
            Err(anyhow::anyhow!(msg))
        }
    }
}

/// 处理消息
async fn handle_message(
    text: &str,
    state: &Arc<AppState>,
    heartbeat: &Arc<Mutex<Instant>>,
    tx: &mpsc::Sender<Message>,
) -> anyhow::Result<()> {
    let message: serde_json::Value = match serde_json::from_str(text) {
        Ok(m) => m,
        Err(e) => {
            send_error(tx, &format!("JSON 解析失败: {}", e)).await;
            return Ok(());
        }
    };

    let msg_type = message["type"].as_str().unwrap_or("");
    let data = message["data"].as_str().unwrap_or("");

    match msg_type {
        "command" => {
            if let Some(pwm_str) = data.strip_prefix("S:") {
                match pwm_str.trim().parse::<u8>() {
                    Ok(speed) => {
                        let seq = state.packet_seq.fetch_add(1, Ordering::Relaxed);
                        let packet = build_wireless_packet(2, 0, speed, seq);
                        if send_packet_blocking(state, packet, tx, "命令发送失败")
                            .await
                            .is_err()
                        {
                            return Ok(());
                        }
                        state.log_command_forward(b'S');
                        return Ok(());
                    }
                    Err(_) => {
                        send_error(tx, &format!("PWM 值无效: {}", pwm_str)).await;
                        return Ok(());
                    }
                }
            }

            if data.len() > 1 {
                send_error(tx, "仅接受单字符命令或 'S:<pwm>' 格式").await;
                return Ok(());
            }

            let cmd_byte = match data.bytes().next() {
                Some(b) => b,
                None => {
                    send_error(tx, "command 消息缺少命令字符").await;
                    return Ok(());
                }
            };

            let (type_value, packet_data, packet_speed) = match cmd_byte {
                b'W' | b'A' | b'S' | b'D' | b'Q' | b'E' | b' ' => {
                    let speed = state.current_speed.load(Ordering::Relaxed);
                    (1, cmd_byte, speed)
                }
                b'B' => (10, 0, 0),
                b'P' => (11, 0, 0),
                _ => {
                    send_error(tx, &format!("未知命令字符: {}", cmd_byte as char)).await;
                    return Ok(());
                }
            };

            let seq = state.packet_seq.fetch_add(1, Ordering::Relaxed);
            let packet = build_wireless_packet(type_value, packet_data, packet_speed, seq);

            if send_packet_blocking(state, packet, tx, "命令发送失败")
                .await
                .is_err()
            {
                return Ok(());
            }
            state.log_command_forward(cmd_byte);
        }
        "speed" => {
            // 速度设置（0-255 PWM）：生成 SPEED 数据包并通过串口发送
            let speed = match data.parse::<u8>() {
                Ok(s) => s,
                Err(_) => {
                    send_error(tx, &format!("速度值非法或越界: {}", data)).await;
                    return Ok(());
                }
            };

            let seq = state.packet_seq.fetch_add(1, Ordering::Relaxed);
            let packet = build_wireless_packet(2, 0, speed, seq);
            if send_packet_blocking(state, packet, tx, "命令发送失败")
                .await
                .is_err()
            {
                return Ok(());
            }
            state.current_speed.store(speed, Ordering::Relaxed);
            info!("设置速度: {}", speed);
        }
        "heartbeat" => {
            let updated = {
                if let Ok(mut last) = heartbeat.try_lock() {
                    *last = Instant::now();
                    true
                } else {
                    false
                }
            };
            if !updated {
                tokio::task::yield_now().await;
                if let Ok(mut last) = heartbeat.try_lock() {
                    *last = Instant::now();
                }
            }
        }
        "drive_mode" => {
            // 行走模式切换：生成 DRIVE_MODE 数据包并通过串口发送
            let mode = match message["mode"].as_u64() {
                Some(m) if m <= 2 => m as u8,
                Some(_) => {
                    send_error(tx, "行走模式值越界，仅支持 0/1/2").await;
                    return Ok(());
                }
                None => {
                    send_error(tx, "drive_mode 消息缺少有效的 mode 字段").await;
                    return Ok(());
                }
            };

            let seq = state.packet_seq.fetch_add(1, Ordering::Relaxed);
            let packet = build_wireless_packet(9, mode, 0, seq);
            if send_packet_blocking(state, packet, tx, "行走模式命令发送失败")
                .await
                .is_err()
            {
                return Ok(());
            }
            state.current_drive_mode.store(mode, Ordering::Relaxed);
            info!("切换行走模式: {}", mode);
        }
        "ble_scan" => {
            // 触发接收器 BLE 扫描：通过串口发送 BLE_SCAN 数据包
            let seq = state.packet_seq.fetch_add(1, Ordering::Relaxed);
            let packet = build_wireless_packet(10, 0, 0, seq);
            if send_packet_blocking(state, packet, tx, "BLE 扫描命令发送失败")
                .await
                .is_err()
            {
                return Ok(());
            }
            info!("已触发 BLE 扫描");
        }
        _ => {
            send_error(tx, &format!("未知消息类型: {}", msg_type)).await;
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 测试 WebSocketManager 初始状态
    #[test]
    fn test_ws_manager_new() {
        let manager = WebSocketManager::new();
        assert_eq!(manager.client_count(), 0);
        assert_eq!(manager.frames_broadcasted(), 0);
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

    /// 测试移除客户端递减计数器
    #[test]
    fn test_remove_nonexistent_client() {
        let mut manager = WebSocketManager::new();
        manager.add_client();
        manager.remove_client(999);
        assert_eq!(manager.client_count(), 1);
    }

    /// 测试 frames_broadcasted 计数
    #[test]
    fn test_frames_broadcasted_count() {
        let mut manager = WebSocketManager::new();
        assert_eq!(manager.increment_frames_broadcasted(), 1);
        assert_eq!(manager.increment_frames_broadcasted(), 2);
        assert_eq!(manager.frames_broadcasted(), 2);
    }

    /// 辅助函数：创建测试用 AppState（认证禁用）
    fn create_test_state() -> Arc<AppState> {
        Arc::new(AppState::new_test())
    }

    /// 辅助函数：调用 handle_message 并收集通过 tx 发送的消息
    async fn call_handle_message(
        state: &Arc<AppState>,
        msg: &str,
    ) -> (anyhow::Result<()>, Vec<Message>) {
        let heartbeat = Arc::new(Mutex::new(Instant::now()));
        let (tx, mut rx) = mpsc::channel::<Message>(MPSC_CAPACITY);
        let result = handle_message(msg, state, &heartbeat, &tx).await;
        drop(tx);
        let mut messages = Vec::new();
        while let Some(m) = rx.recv().await {
            messages.push(m);
        }
        (result, messages)
    }

    /// 测试 handle_message 处理命令消息（无串口连接时返回错误消息但不关闭连接）
    #[tokio::test]
    async fn test_handle_message_command() {
        let state = create_test_state();
        let msg = r#"{"type":"command","data":"W"}"#;

        let (result, messages) = call_handle_message(&state, msg).await;
        assert!(
            result.is_ok(),
            "无串口连接时 handle_message 应返回 Ok: {:?}",
            result
        );
        assert!(
            messages
                .iter()
                .any(|m| matches!(m, Message::Text(t) if t.contains("命令发送失败"))),
            "无串口连接时应收到 error 消息"
        );
    }

    /// 测试 handle_message 处理未知命令字符时返回错误消息
    #[tokio::test]
    async fn test_handle_message_unknown_command() {
        let state = create_test_state();
        let msg = r#"{"type":"command","data":"X"}"#;

        let (result, messages) = call_handle_message(&state, msg).await;
        assert!(result.is_ok(), "未知命令不应导致 handle_message 返回 Err");
        assert!(
            messages
                .iter()
                .any(|m| matches!(m, Message::Text(t) if t.contains("未知命令字符"))),
            "未知命令应收到 error 消息"
        );
    }

    /// 测试 handle_message 处理速度设置命令（0-255 PWM），无串口时返回错误消息但不关闭连接
    #[tokio::test]
    async fn test_handle_message_speed_command_updates_state() {
        let state = create_test_state();
        let msg = r#"{"type":"speed","data":"200"}"#;

        let (result, messages) = call_handle_message(&state, msg).await;
        assert!(
            result.is_ok(),
            "无串口连接时处理速度命令应返回 Ok: {:?}",
            result
        );
        assert!(
            messages
                .iter()
                .any(|m| matches!(m, Message::Text(t) if t.contains("命令发送失败"))),
            "无串口连接时应收到 error 消息"
        );
    }

    /// 测试 handle_message 处理非法速度值时返回错误消息
    #[tokio::test]
    async fn test_handle_message_speed_invalid() {
        let state = create_test_state();

        // 超出 u8 范围
        let (result, messages) =
            call_handle_message(&state, r#"{"type":"speed","data":"256"}"#).await;
        assert!(result.is_ok(), "非法速度不应导致 handle_message 返回 Err");
        assert!(
            messages
                .iter()
                .any(|m| matches!(m, Message::Text(t) if t.contains("速度值非法"))),
            "非法速度应收到 error 消息"
        );

        // 非数字
        let (result, messages) =
            call_handle_message(&state, r#"{"type":"speed","data":"fast"}"#).await;
        assert!(result.is_ok());
        assert!(messages
            .iter()
            .any(|m| matches!(m, Message::Text(t) if t.contains("速度值非法"))));
    }

    /// 测试 handle_message 处理行走模式切换消息（无串口连接时返回错误消息但不关闭连接）
    #[tokio::test]
    async fn test_handle_message_drive_mode() {
        let state = create_test_state();

        for mode in [0, 1, 2] {
            let msg = format!(r#"{{"type":"drive_mode","mode":{}}}"#, mode);
            let (result, messages) = call_handle_message(&state, &msg).await;
            assert!(
                result.is_ok(),
                "无串口连接时 handle_message 处理行走模式 {} 应返回 Ok: {:?}",
                mode,
                result
            );
            assert!(
                messages.iter().any(|m| matches!(m, Message::Text(t)
                    if t.contains("行走模式命令发送失败"))),
                "无串口连接时应收到 error 消息"
            );
        }
    }

    /// 测试 handle_message 处理非法行走模式值时返回错误消息
    #[tokio::test]
    async fn test_handle_message_drive_mode_invalid() {
        let state = create_test_state();

        // 越界
        let (result, messages) =
            call_handle_message(&state, r#"{"type":"drive_mode","mode":3}"#).await;
        assert!(result.is_ok(), "越界模式不应导致 handle_message 返回 Err");
        assert!(
            messages
                .iter()
                .any(|m| matches!(m, Message::Text(t) if t.contains("越界"))),
            "越界模式应收到 error 消息"
        );

        // 缺少 mode 字段
        let (result, messages) = call_handle_message(&state, r#"{"type":"drive_mode"}"#).await;
        assert!(result.is_ok());
        assert!(
            messages
                .iter()
                .any(|m| matches!(m, Message::Text(t) if t.contains("缺少有效的 mode"))),
            "缺少 mode 应收到 error 消息"
        );
    }

    /// 测试 handle_message 处理无效 JSON（返回错误消息但不关闭连接）
    #[tokio::test]
    async fn test_handle_message_invalid_json() {
        let state = create_test_state();
        let msg = "这不是有效的JSON";

        let (result, messages) = call_handle_message(&state, msg).await;
        assert!(
            result.is_ok(),
            "handle_message 处理无效 JSON 时不应关闭连接: {:?}",
            result
        );
        assert!(
            messages
                .iter()
                .any(|m| matches!(m, Message::Text(t) if t.contains("JSON 解析失败"))),
            "无效 JSON 应收到 error 消息"
        );
    }

    /// 测试 handle_message 处理未知消息类型时通过 error 消息响应
    #[tokio::test]
    async fn test_handle_message_unknown_type() {
        let state = create_test_state();
        let msg = r#"{"type":"unknown_type","data":"test"}"#;

        let (result, messages) = call_handle_message(&state, msg).await;
        assert!(
            result.is_ok(),
            "handle_message 处理未知消息类型时应正常返回: {:?}",
            result
        );
        assert!(
            messages
                .iter()
                .any(|m| matches!(m, Message::Text(t) if t.contains("未知消息类型"))),
            "未知消息类型应收到 error 消息"
        );
    }

    /// 测试 handle_message 处理心跳消息（验证按客户端心跳时间被更新）
    #[tokio::test]
    async fn test_handle_message_heartbeat() {
        let state = create_test_state();
        let heartbeat = Arc::new(Mutex::new(Instant::now() - Duration::from_secs(1)));
        let (tx, mut rx) = mpsc::channel::<Message>(MPSC_CAPACITY);

        let msg = r#"{"type":"heartbeat"}"#;
        let result = handle_message(msg, &state, &heartbeat, &tx).await;
        assert!(
            result.is_ok(),
            "handle_message 处理心跳消息时不应返回错误: {:?}",
            result
        );

        // 验证按客户端心跳时间已更新
        let updated = *heartbeat.lock().expect("心跳锁不应中毒");
        assert!(
            updated.elapsed() < Duration::from_millis(100),
            "心跳时间应在处理后更新"
        );

        // 心跳消息不应产生任何回复
        drop(tx);
        assert!(rx.recv().await.is_none(), "心跳不应发送消息");
    }

    /// 测试多客户端并发添加到 WebSocketManager，通过 Mutex 保护的正确性
    #[tokio::test]
    async fn test_multiple_clients_concurrent() {
        let manager = Arc::new(Mutex::new(WebSocketManager::new()));
        let mut handles = Vec::new();

        for _ in 0..10 {
            let mgr = manager.clone();
            let handle = tokio::spawn(async move {
                let mut guard = mgr.lock().expect("manager lock");
                guard.add_client()
            });
            handles.push(handle);
        }

        let mut ids = Vec::new();
        for handle in handles {
            let id = handle.await.expect("并发添加客户端任务不应 panic");
            ids.push(id);
        }

        ids.sort();
        ids.dedup();
        assert_eq!(ids.len(), 10, "10 个并发客户端应分配到 10 个唯一 ID");

        let count = manager.lock().expect("manager lock").client_count();
        assert_eq!(count, 10, "并发添加后客户端总数应为 10");
    }

    /// 测试并发添加和移除客户端的正确性
    #[tokio::test]
    async fn test_concurrent_add_and_remove() {
        let manager = Arc::new(Mutex::new(WebSocketManager::new()));

        let mut ids = Vec::new();
        for _ in 0..5 {
            let mut guard = manager.lock().expect("manager lock");
            ids.push(guard.add_client());
        }

        let mut handles = Vec::new();
        for &id in &ids[..3] {
            let mgr = manager.clone();
            let handle = tokio::spawn(async move {
                let mut guard = mgr.lock().expect("manager lock");
                guard.remove_client(id)
            });
            handles.push(handle);
        }

        for handle in handles {
            handle.await.expect("并发移除客户端任务不应 panic");
        }

        let count = manager.lock().expect("manager lock").client_count();
        assert_eq!(count, 2, "移除 3 个后应剩余 2 个客户端");
    }

    /// 测试 handle_message 处理 ble_scan 消息（无串口连接时返回错误消息但不关闭连接）
    #[tokio::test]
    async fn test_handle_message_ble_scan() {
        let state = create_test_state();
        let msg = r#"{"type":"ble_scan"}"#;

        let (result, messages) = call_handle_message(&state, msg).await;
        assert!(
            result.is_ok(),
            "无串口连接时 handle_message 处理 ble_scan 应返回 Ok: {:?}",
            result
        );
        assert!(
            messages
                .iter()
                .any(|m| matches!(m, Message::Text(t) if t.contains("BLE 扫描命令发送失败"))),
            "无串口连接时应收到 error 消息"
        );
    }
}
