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
use base64::Engine;
use futures::{sink::SinkExt, stream::StreamExt};
use tokio::sync::mpsc;
use tokio_util::sync::CancellationToken;
use tracing::{debug, error, info, warn};

use crate::AppState;

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
        let mut manager = state.ws_manager.lock().unwrap();
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
            let mut manager = state.ws_manager.lock().unwrap();
            manager.remove_client(client_id);
        }
        return;
    }

    // 创建取消令牌，用于优雅关闭视频广播任务
    let cancel_token = CancellationToken::new();

    // 视频任务：通过 mpsc tx 发送视频帧
    let video_tx = tx.clone();
    let video_state = state.clone();
    let video_cancel = cancel_token.clone();
    let video_task = tokio::spawn(async move {
        let mut last_frame_hash: Option<u64> = None;
        let mut last_odometry_send = Instant::now();

        loop {
            // 检查取消信号
            if video_cancel.is_cancelled() {
                debug!("视频广播任务收到取消信号，优雅退出");
                break;
            }

            // 获取视频帧（使用 Arc::clone 共享引用，避免 clone 整帧数据）
            let frame = {
                let video = video_state.video_frame.lock().unwrap();
                video.as_ref().map(Arc::clone)
            };

            if let Some(ref frame_data) = frame {
                // 使用多点采样哈希（帧长度 + 前4字节 + 中4字节 + 末4字节）判断帧是否更新
                // 避免仅用首字节（JPEG 固定 0xFF）导致同尺寸帧哈希碰撞
                let hash = {
                    let len = frame_data.len() as u64;
                    let first4 = frame_data
                        .get(0..4)
                        .map(|s| u32::from_be_bytes(s.try_into().unwrap_or([0; 4])) as u64)
                        .unwrap_or(0);
                    let mid4 = {
                        let mid = frame_data.len() / 2;
                        frame_data
                            .get(mid..mid + 4)
                            .map(|s| u32::from_be_bytes(s.try_into().unwrap_or([0; 4])) as u64)
                            .unwrap_or(0)
                    };
                    let last4 = if frame_data.len() >= 8 {
                        frame_data
                            .get(frame_data.len() - 4..)
                            .map(|s| u32::from_be_bytes(s.try_into().unwrap_or([0; 4])) as u64)
                            .unwrap_or(0)
                    } else {
                        0u64
                    };
                    len ^ (first4 << 32) ^ (mid4 << 16) ^ last4
                };

                if last_frame_hash != Some(hash) {
                    last_frame_hash = Some(hash);

                    // 编码为Base64
                    let base64 = base64_encode(frame_data);

                    let message = serde_json::json!({
                        "type": "video",
                        "format": "jpeg",
                        "data": base64,
                        "timestamp": chrono::Utc::now().timestamp_millis()
                    });

                    if let Err(e) = video_tx
                        .send(Message::Text(message.to_string().into()))
                        .await
                    {
                        debug!("视频发送失败: {}", e);
                        break;
                    }
                }
            }

            // 发送测速数据（固件 200ms 上报一次，此处限流避免冗余发送）
            if last_odometry_send.elapsed() >= std::time::Duration::from_millis(200) {
                last_odometry_send = Instant::now();
                let message = {
                    let odom = video_state.odometry.lock().await;
                    serde_json::json!({
                        "type": "odometry",
                        "leftSpeed": odom.left_speed_mmps,
                        "rightSpeed": odom.right_speed_mmps,
                        "heading": odom.heading,
                        "distance": odom.total_distance_mm,
                        "timestamp": chrono::Utc::now().timestamp_millis()
                    })
                }; // odom 锁在此处释放

                if let Err(e) = video_tx
                    .send(Message::Text(message.to_string().into()))
                    .await
                {
                    debug!("测速数据发送失败: {}", e);
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
    let _ = video_task.await;
    // 关闭转发通道（drop tx 触发 rx 结束）
    drop(tx);
    let _ = forward_task.await;

    // 注销客户端
    {
        let mut manager = state.ws_manager.lock().unwrap();
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
                    let mut manager = state.serial_manager.lock().unwrap();
                    if let Err(e) = manager.send_command(cmd_byte) {
                        warn!("发送命令失败: {}", e);
                    } else {
                        debug!("转发命令: {}", data);
                    }
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
            // 设置速度
            if let Ok(speed) = data.parse::<u8>() {
                state.current_speed.store(speed, Ordering::Relaxed);
                info!("设置速度: {}", speed);
            }
        }
        "heartbeat" => {
            // 心跳
            let mut last = state.last_heartbeat.lock().unwrap();
            *last = std::time::Instant::now();
        }
        "drive_mode" => {
            // 行走模式切换：发送 DRIVE_MODE 命令类型 + 模式值
            // 车端 handleDriveModeCommand 接收 packet->data 作为模式值
            // 协议：先发 'M'/'L'/'B' 标识类型，再发模式值 0/1/2
            if let Some(mode) = message["mode"].as_u64() {
                {
                    let mut manager = state.serial_manager.lock().unwrap();
                    // 发送模式标识字符
                    let mode_char = match mode {
                        0 => 'M', // 普通模式
                        1 => 'L', // 直线修正模式
                        2 => 'B', // 航向锁定模式
                        _ => 'L',
                    };
                    let _ = manager.send_command(mode_char as u8);
                    // 发送模式数值（0/1/2），车端通过 DRIVE_MODE 类型解析
                    let _ = manager.send_command(mode as u8);
                }
                info!("切换行走模式: {}", mode);
            }
        }
        _ => {
            warn!("未知消息类型: {}", msg_type);
        }
    }

    Ok(())
}

/// Base64编码
fn base64_encode(data: &[u8]) -> String {
    base64::engine::general_purpose::STANDARD.encode(data)
}

#[cfg(test)]
mod tests {
    use super::*;

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
        let encoded = base64_encode(&[0x00, 0x01, 0x02]);
        assert_eq!(encoded, "AAEC");
    }

    /// 测试 handle_message 处理命令消息（无串口连接时不 panic，函数正常返回）
    #[tokio::test]
    async fn test_handle_message_command() {
        let state = create_test_state();
        let msg = r#"{"type":"command","data":"W"}"#;

        // 无串口连接时，send_command 会失败但 handle_message 本身应正常返回 Ok
        let result = handle_message(msg, &state).await;
        assert!(
            result.is_ok(),
            "handle_message 处理命令消息时不应返回错误: {:?}",
            result
        );
    }

    /// 测试 handle_message 处理速度等级命令（'1'-'9'），验证 current_speed 被更新
    #[tokio::test]
    async fn test_handle_message_speed_command_updates_state() {
        let state = create_test_state();
        let msg = r#"{"type":"command","data":"7"}"#;

        let result = handle_message(msg, &state).await;
        assert!(result.is_ok(), "处理速度命令不应返回错误: {:?}", result);

        // 验证速度已更新为 7
        let speed = state.current_speed.load(Ordering::Relaxed);
        assert_eq!(speed, 7, "速度等级应更新为 7");
    }

    /// 测试 handle_message 处理行走模式切换消息（无串口连接时不 panic）
    #[tokio::test]
    async fn test_handle_message_drive_mode() {
        let state = create_test_state();

        // 测试普通模式
        let msg = r#"{"type":"drive_mode","mode":0}"#;
        let result = handle_message(msg, &state).await;
        assert!(
            result.is_ok(),
            "handle_message 处理行走模式消息时不应返回错误: {:?}",
            result
        );

        // 测试直线修正模式
        let msg = r#"{"type":"drive_mode","mode":1}"#;
        let result = handle_message(msg, &state).await;
        assert!(
            result.is_ok(),
            "handle_message 处理直线修正模式时不应返回错误: {:?}",
            result
        );

        // 测试航向锁定模式
        let msg = r#"{"type":"drive_mode","mode":2}"#;
        let result = handle_message(msg, &state).await;
        assert!(
            result.is_ok(),
            "handle_message 处理航向锁定模式时不应返回错误: {:?}",
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
        let initial = *state.last_heartbeat.lock().unwrap();

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
        let updated = *state.last_heartbeat.lock().unwrap();
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
}
