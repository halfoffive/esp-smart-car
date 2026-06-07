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
use std::sync::Arc;

use axum::{
    extract::ws::{Message, WebSocket, WebSocketUpgrade},
    extract::State,
    response::IntoResponse,
};
use base64::Engine;
use futures::{sink::SinkExt, stream::StreamExt};
use tokio::sync::mpsc;
use tracing::{debug, error, info, warn};

use crate::AppState;

/// 客户端连接
#[derive(Debug)]
struct ClientConnection {
    id: u64,
    #[allow(dead_code)]
    connected_at: std::time::Instant,
}

/// WebSocket管理器
pub struct WebSocketManager {
    /// 已连接客户端
    clients: Vec<ClientConnection>,
    /// 下一个客户端ID
    next_id: u64,
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

        self.clients.push(ClientConnection {
            id,
            connected_at: std::time::Instant::now(),
        });

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
        let mut manager = state.ws_manager.lock().await;
        manager.add_client()
    };

    // 拆分 WebSocket 为发送/接收
    let (mut ws_sender, mut ws_receiver) = socket.split();

    // 创建 mpsc 通道用于发送消息（tx 可 clone）
    let (tx, mut rx) = mpsc::channel::<Message>(32);

    // 转发任务：从 mpsc 通道接收消息并发送到 WebSocket
    let forward_task = tokio::spawn(async move {
        while let Some(msg) = rx.recv().await {
            if ws_sender.send(msg).await.is_err() {
                break;
            }
        }
    });

    // 发送欢迎消息
    let welcome = serde_json::json!({
        "type": "connected",
        "client_id": client_id,
        "message": "已连接到智能车控制系统"
    });

    if let Err(e) = tx.send(Message::Text(welcome.to_string().into())).await {
        error!("发送欢迎消息失败: {}", e);
        forward_task.abort();
        {
            let mut manager = state.ws_manager.lock().await;
            manager.remove_client(client_id);
        }
        return;
    }

    // 视频任务：通过 mpsc tx 发送视频帧
    let video_tx = tx.clone();
    let video_state = state.clone();
    let video_task = tokio::spawn(async move {
        loop {
            // 获取视频帧
            let frame = {
                let video = video_state.video_frame.lock().await;
                video.clone()
            };

            if let Some(frame_data) = frame {
                // 编码为Base64
                let base64 = base64_encode(&frame_data);

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

            // 发送测速数据
            {
                let odom = video_state.odometry.lock().await;
                let message = serde_json::json!({
                    "type": "odometry",
                    "leftSpeed": odom.left_speed_mmps,
                    "rightSpeed": odom.right_speed_mmps,
                    "heading": odom.heading,
                    "distance": odom.total_distance_mm,
                    "timestamp": chrono::Utc::now().timestamp_millis()
                });

                if let Err(e) = video_tx
                    .send(Message::Text(message.to_string().into()))
                    .await
                {
                    debug!("测速数据发送失败: {}", e);
                }
            }

            // 控制帧率
            tokio::time::sleep(std::time::Duration::from_millis(33)).await; // ~30 FPS
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

    // 取消视频任务
    video_task.abort();
    // 取消转发任务
    forward_task.abort();

    // 注销客户端
    {
        let mut manager = state.ws_manager.lock().await;
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
                // 如果是速度等级命令(1-9)，同步更新 current_speed
                if cmd_byte >= b'1' && cmd_byte <= b'9' {
                    let mut current_speed = state.current_speed.lock().await;
                    *current_speed = cmd_byte - b'0';
                }
                let mut manager = state.serial_manager.lock().await;
                if let Err(e) = manager.send_command(cmd_byte) {
                    warn!("发送命令失败: {}", e);
                } else {
                    debug!("转发命令: {}", data);
                }
            }
        }
        "speed" => {
            // 设置速度
            if let Ok(speed) = data.parse::<u8>() {
                let mut current_speed = state.current_speed.lock().await;
                *current_speed = speed;
                info!("设置速度: {}", speed);
            }
        }
        "heartbeat" => {
            // 心跳
            let mut last = state.last_heartbeat.lock().await;
            *last = std::time::Instant::now();
        }
        "drive_mode" => {
            // 行走模式切换
            if let Some(mode) = message["mode"].as_u64() {
                let cmd = match mode {
                    0 => 'M', // 普通模式
                    1 => 'L', // 直线修正模式
                    2 => 'H', // 航向锁定模式
                    _ => 'L',
                };
                let mut manager = state.serial_manager.lock().await;
                let _ = manager.send_command(cmd as u8);
                let mut manager2 = state.serial_manager.lock().await;
                let _ = manager2.send_command(mode as u8);
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
