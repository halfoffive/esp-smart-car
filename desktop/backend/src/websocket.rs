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
use futures::{sink::SinkExt, stream::StreamExt};
use tokio::sync::Mutex;
use tracing::{debug, error, info, warn};

use crate::AppState;

/// 客户端连接
#[derive(Debug)]
struct ClientConnection {
    id: u64,
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
        
        info!("WebSocket客户端连接: #{} (总计: {})", id, self.clients.len());
        id
    }
    
    pub fn remove_client(&mut self, id: u64) {
        self.clients.retain(|c| c.id != id);
        info!("WebSocket客户端断开: #{} (剩余: {})", id, self.clients.len());
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
    
    let (mut sender, mut receiver) = socket.split();
    
    // 发送欢迎消息
    let welcome = serde_json::json!({
        "type": "connected",
        "client_id": client_id,
        "message": "已连接到智能车控制系统"
    });
    
    if let Err(e) = sender.send(Message::Text(welcome.to_string())).await {
        error!("发送欢迎消息失败: {}", e);
        return;
    }
    
    // 创建任务：发送视频帧
    let video_sender = sender.clone();
    let video_state = state.clone();
    let mut video_task = tokio::spawn(async move {
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
                
                if let Err(e) = video_sender.send(Message::Text(message.to_string())).await {
                    debug!("视频发送失败: {}", e);
                    break;
                }
            }
            
            // 控制帧率
            tokio::time::sleep(std::time::Duration::from_millis(33)).await; // ~30 FPS
        }
    });
    
    // 处理接收到的消息
    while let Some(result) = receiver.next().await {
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
                if let Err(e) = sender.send(Message::Pong(data)).await {
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
        _ => {
            warn!("未知消息类型: {}", msg_type);
        }
    }
    
    Ok(())
}

/// 广播视频帧（全局广播）
pub async fn broadcast_video_frames(state: Arc<AppState>) {
    loop {
        // 获取视频帧
        let frame = {
            let video = state.video_frame.lock().await;
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
            
            // 这里可以实现广播逻辑
            // 目前每个WebSocket连接有自己的视频发送任务
        }
        
        tokio::time::sleep(std::time::Duration::from_millis(33)).await;
    }
}

/// Base64编码
fn base64_encode(data: &[u8]) -> String {
    base64::engine::general_purpose::STANDARD.encode(data)
}
