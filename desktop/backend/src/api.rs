/**
 * HTTP API 模块
 * 基于 Axum 框架，提供 RESTful API
 *
 * 端点：
 * POST /api/command - 发送控制命令
 * GET /api/status - 获取系统状态
 * POST /api/connect - 连接串口
 * POST /api/disconnect - 断开串口
 *
 * 数据格式：JSON
 */
use std::sync::Arc;

use axum::{extract::State, http::StatusCode, Json};
use serde::{Deserialize, Serialize};
use tracing::{info, warn};

use crate::serial::{SerialConnectionState, DEFAULT_BAUD_RATE};
use crate::AppState;

/// 命令请求
#[derive(Debug, Deserialize)]
pub struct CommandRequest {
    /// 命令字符
    pub command: String,
    /// 可选速度参数
    #[allow(dead_code)]
    pub speed: Option<u8>,
}

/// 状态响应
#[derive(Debug, Serialize)]
pub struct StatusResponse {
    /// 串口连接状态
    pub serial_status: String,
    /// 串口端口名
    pub port_name: Option<String>,
    /// 串口波特率
    pub baud_rate: Option<u32>,
    /// 已接收帧数
    pub frame_count: u32,
    /// 已发送字节数
    pub bytes_sent: u64,
    /// 当前速度
    pub current_speed: u8,
    /// WebSocket连接数
    pub ws_clients: usize,
    /// 运行时间（秒）
    pub uptime: u64,
    /// 系统版本
    pub version: String,
    /// 左轮速度(mm/s)
    pub left_speed: f32,
    /// 右轮速度(mm/s)
    pub right_speed: f32,
    /// 航向角(弧度)
    pub heading: f32,
    /// 总行走距离(mm)
    pub total_distance: f32,
    /// 已发送命令数
    pub command_count: u64,
}

/// 串口连接请求
#[derive(Debug, Deserialize)]
pub struct ConnectRequest {
    /// 串口名称
    pub port_name: String,
    /// 波特率（可选，默认 921600）
    pub baud_rate: Option<u32>,
}

/// 通用响应
#[derive(Debug, Serialize)]
pub struct ApiResponse {
    /// 是否成功
    pub success: bool,
    /// 消息
    pub message: String,
}

/// 处理命令
pub async fn handle_command(
    State(state): State<Arc<AppState>>,
    Json(request): Json<CommandRequest>,
) -> (StatusCode, Json<ApiResponse>) {
    let cmd = match request.command.as_bytes().first().copied() {
        Some(c) => c,
        None => {
            return (
                StatusCode::BAD_REQUEST,
                Json(ApiResponse {
                    success: false,
                    message: "命令不能为空".to_string(),
                }),
            );
        }
    };

    let mut manager = state.serial_manager.lock().unwrap();

    match manager.send_command(cmd) {
        Ok(()) => {
            info!("发送命令: {}", request.command);
            (
                StatusCode::OK,
                Json(ApiResponse {
                    success: true,
                    message: format!("命令 '{}' 已发送", request.command),
                }),
            )
        }
        Err(e) => {
            warn!("发送命令失败: {}", e);
            (
                StatusCode::SERVICE_UNAVAILABLE,
                Json(ApiResponse {
                    success: false,
                    message: format!("发送失败: {}", e),
                }),
            )
        }
    }
}

/// 获取系统状态
pub async fn get_status(State(state): State<Arc<AppState>>) -> (StatusCode, Json<StatusResponse>) {
    // 逐把加锁，复制数据后立即释放，减少锁争用
    let (serial_status, port_name, baud_rate, frame_count, bytes_sent, command_count) = {
        let manager = state.serial_manager.lock().unwrap();
        let (serial_status, port_name, baud_rate) = match &manager.state {
            SerialConnectionState::Disconnected => ("未连接".to_string(), None, None),
            SerialConnectionState::Connecting => ("连接中".to_string(), None, None),
            SerialConnectionState::Connected { port_name, baud_rate } => {
                ("已连接".to_string(), Some(port_name.clone()), Some(*baud_rate))
            }
            SerialConnectionState::Error(msg) => {
                (format!("错误: {}", msg), None, None)
            }
        };
        (
            serial_status,
            port_name,
            baud_rate,
            manager.frame_count,
            manager.bytes_sent,
            manager.command_count,
        )
    };

    let ws_clients = {
        let ws_manager = state.ws_manager.lock().await;
        ws_manager.client_count()
    };

    let current_speed = *state.current_speed.lock().await;

    let (left_speed, right_speed, heading, total_distance) = {
        let odom = state.odometry.lock().await;
        (
            odom.left_speed_mmps,
            odom.right_speed_mmps,
            odom.heading,
            odom.total_distance_mm,
        )
    };

    let uptime = state.started_at.elapsed().as_secs();

    let response = StatusResponse {
        serial_status,
        port_name,
        baud_rate,
        frame_count,
        bytes_sent,
        current_speed,
        ws_clients,
        uptime,
        version: env!("CARGO_PKG_VERSION").to_string(),
        left_speed,
        right_speed,
        heading,
        total_distance,
        command_count,
    };

    (StatusCode::OK, Json(response))
}

/// 连接串口
pub async fn connect_serial(
    State(state): State<Arc<AppState>>,
    Json(request): Json<ConnectRequest>,
) -> (StatusCode, Json<ApiResponse>) {
    let baud_rate = request.baud_rate.unwrap_or(DEFAULT_BAUD_RATE);

    let mut manager = state.serial_manager.lock().unwrap();

    // 先断开现有连接
    manager.disconnect();

    match manager.connect(&request.port_name, baud_rate) {
        Ok(()) => {
            info!("串口连接成功: {} @ {}", request.port_name, baud_rate);
            (
                StatusCode::OK,
                Json(ApiResponse {
                    success: true,
                    message: format!("已连接到 {}", request.port_name),
                }),
            )
        }
        Err(e) => {
            warn!("串口连接失败: {}", e);
            (
                StatusCode::SERVICE_UNAVAILABLE,
                Json(ApiResponse {
                    success: false,
                    message: format!("连接失败: {}", e),
                }),
            )
        }
    }
}

/// 断开串口
pub async fn disconnect_serial(
    State(state): State<Arc<AppState>>,
) -> (StatusCode, Json<ApiResponse>) {
    let mut manager = state.serial_manager.lock().unwrap();
    manager.disconnect();

    info!("串口已断开");

    (
        StatusCode::OK,
        Json(ApiResponse {
            success: true,
            message: "串口已断开".to_string(),
        }),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 测试 CommandRequest 反序列化（含 speed）
    #[test]
    fn test_command_request_with_speed() {
        let json = r#"{"command":"W","speed":5}"#;
        let req: CommandRequest = serde_json::from_str(json).unwrap();
        assert_eq!(req.command, "W");
        assert_eq!(req.speed, Some(5));
    }

    /// 测试 CommandRequest 反序列化（无 speed）
    #[test]
    fn test_command_request_without_speed() {
        let json = r#"{"command":"S"}"#;
        let req: CommandRequest = serde_json::from_str(json).unwrap();
        assert_eq!(req.command, "S");
        assert_eq!(req.speed, None);
    }

    /// 测试 ConnectRequest 反序列化（含 baud_rate）
    #[test]
    fn test_connect_request_with_baud_rate() {
        let json = r#"{"port_name":"COM3","baud_rate":115200}"#;
        let req: ConnectRequest = serde_json::from_str(json).unwrap();
        assert_eq!(req.port_name, "COM3");
        assert_eq!(req.baud_rate, Some(115200));
    }

    /// 测试 ConnectRequest 反序列化（无 baud_rate）
    #[test]
    fn test_connect_request_without_baud_rate() {
        let json = r#"{"port_name":"/dev/ttyUSB0"}"#;
        let req: ConnectRequest = serde_json::from_str(json).unwrap();
        assert_eq!(req.port_name, "/dev/ttyUSB0");
        assert_eq!(req.baud_rate, None);
    }

    /// 测试 ApiResponse 序列化
    #[test]
    fn test_api_response_serialize() {
        let resp = ApiResponse {
            success: true,
            message: "ok".to_string(),
        };
        let json = serde_json::to_string(&resp).unwrap();
        assert!(json.contains("\"success\":true"));
        assert!(json.contains("\"message\":\"ok\""));
    }

    /// 测试 StatusResponse 序列化
    #[test]
    fn test_status_response_serialize() {
        let resp = StatusResponse {
            serial_status: "未连接".to_string(),
            port_name: None,
            baud_rate: None,
            frame_count: 0,
            bytes_sent: 0,
            current_speed: 5,
            ws_clients: 0,
            uptime: 42,
            version: "1.2.0".to_string(),
            left_speed: 0.0,
            right_speed: 0.0,
            heading: 0.0,
            total_distance: 0.0,
            command_count: 0,
        };
        let json = serde_json::to_string(&resp).unwrap();
        assert!(json.contains("\"serial_status\":\"未连接\""));
        assert!(json.contains("\"current_speed\":5"));
        assert!(json.contains("\"version\":\"1.2.0\""));
    }
}
