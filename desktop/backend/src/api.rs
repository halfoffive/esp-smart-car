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
use std::sync::atomic::Ordering;
use std::sync::Arc;

use axum::{extract::State, http::StatusCode, Json};
use serde::{Deserialize, Serialize};
use tracing::{info, warn};

use crate::serial::{SerialConnectionState, DEFAULT_BAUD_RATE};
use crate::websocket::build_wireless_packet;
use crate::{AppState, MutexExt};

/// 命令请求
#[derive(Debug, Deserialize)]
pub struct CommandRequest {
    /// 命令字符
    pub command: String,
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
    /// 当前速度 PWM 值（0-255）
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

/// 列出可用串口（使用 AppState 缓存的串口列表，避免每次请求都执行 spawn_blocking）
pub async fn list_ports(
    State(state): State<Arc<AppState>>,
) -> (StatusCode, Json<serde_json::Value>) {
    // 使用 AppState 缓存的串口列表，避免每次请求都执行 spawn_blocking
    let ports = state.available_ports.lock().await.clone();

    (
        StatusCode::OK,
        Json(serde_json::json!({
            "success": true,
            "ports": ports,
        })),
    )
}

/// 处理命令
pub async fn handle_command(
    State(state): State<Arc<AppState>>,
    Json(request): Json<CommandRequest>,
) -> (StatusCode, Json<ApiResponse>) {
    // 处理 S:<pwm> 速度设置命令
    if let Some(pwm_str) = request.command.strip_prefix("S:") {
        match pwm_str.trim().parse::<u8>() {
            Ok(speed) => {
                let seq = state.packet_seq.fetch_add(1, Ordering::Relaxed);
                let packet = build_wireless_packet(2, 0, speed, seq);
                let send_result = {
                    let mut manager = state.serial_manager.lock_or_recover("serial_manager");
                    manager.send_packet(&packet)
                };

                return match send_result {
                    Ok(()) => {
                        state.current_speed.store(speed, Ordering::Relaxed);
                        info!("设置速度 PWM: {}", speed);
                        (
                            StatusCode::OK,
                            Json(ApiResponse {
                                success: true,
                                message: format!("速度已设置为 {}", speed),
                            }),
                        )
                    }
                    Err(e) => {
                        warn!("发送速度命令失败: {}", e);
                        (
                            StatusCode::SERVICE_UNAVAILABLE,
                            Json(ApiResponse {
                                success: false,
                                message: format!("发送失败: {}", e),
                            }),
                        )
                    }
                };
            }
            Err(_) => {
                warn!("S: 命令中的 PWM 值无效: {}", pwm_str);
                return (
                    StatusCode::BAD_REQUEST,
                    Json(ApiResponse {
                        success: false,
                        message: format!("PWM 值无效: {}", pwm_str),
                    }),
                );
            }
        }
    }

    // 单字符命令
    let cmd = match request.command.as_bytes().first().copied() {
        Some(c) => c,
        None => {
            warn!("收到空命令请求");
            return (
                StatusCode::BAD_REQUEST,
                Json(ApiResponse {
                    success: false,
                    message: "命令不能为空".to_string(),
                }),
            );
        }
    };

    let (type_value, data, speed) = match cmd {
        b'W' | b'A' | b'S' | b'D' | b'Q' | b'E' | b' ' => {
            let speed = state.current_speed.load(Ordering::Relaxed);
            (1, cmd, speed)
        }
        b'B' => (10, 0, 0),
        b'P' => (11, 0, 0),
        _ => {
            warn!("未知命令: {}", request.command);
            return (
                StatusCode::BAD_REQUEST,
                Json(ApiResponse {
                    success: false,
                    message: format!("未知命令: {}", request.command),
                }),
            );
        }
    };

    let seq = state.packet_seq.fetch_add(1, Ordering::Relaxed);
    let packet = build_wireless_packet(type_value, data, speed, seq);

    // 在独立作用域中发送命令，确保 std::sync::MutexGuard 在 .await 前释放
    let send_result = {
        let mut manager = state.serial_manager.lock_or_recover("serial_manager");
        manager.send_packet(&packet)
    }; // manager 锁在此处释放

    match send_result {
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
        let manager = state.serial_manager.lock_or_recover("serial_manager");
        let (serial_status, port_name, baud_rate) = match &manager.state {
            SerialConnectionState::Disconnected => ("未连接".to_string(), None, None),
            SerialConnectionState::Connecting => ("连接中".to_string(), None, None),
            SerialConnectionState::Connected {
                port_name,
                baud_rate,
            } => (
                "已连接".to_string(),
                Some(port_name.clone()),
                Some(*baud_rate),
            ),
            SerialConnectionState::Error(msg) => (format!("错误: {}", msg), None, None),
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
        let ws_manager = state.ws_manager.lock_or_recover("ws_manager");
        ws_manager.client_count()
    };

    let current_speed = state.current_speed.load(Ordering::Relaxed);

    let (left_speed, right_speed, heading, total_distance) = {
        let odom = state.odometry.lock_or_recover("odometry");
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
    let port_name = request.port_name;

    // 先断开现有连接（在锁内）
    {
        let mut manager = state.serial_manager.lock_or_recover("serial_manager");
        manager.disconnect();
    } // 释放锁

    // 在 spawn_blocking 中执行阻塞 I/O（serialport::open 是阻塞调用）
    let state_clone = Arc::clone(&state);
    let port_name_clone = port_name.clone();
    let connect_result = tokio::task::spawn_blocking(move || {
        let mut manager = state_clone.serial_manager.lock_or_recover("serial_manager");
        manager.connect(&port_name_clone, baud_rate)
    })
    .await;

    // 根据结果更新状态
    match connect_result {
        Ok(Ok(())) => {
            info!("串口连接成功: {} @ {}", port_name, baud_rate);
            // 连接成功后立即发送 LINK_STATUS 探测包，触发 Dongle 上报链路状态 JSON
            // 用户可感知"连接 = 链路打通"，避免连接后无反馈
            {
                let seq = state.packet_seq.fetch_add(1, Ordering::Relaxed);
                let packet = build_wireless_packet(11, 0, 0, seq);
                let mut manager = state.serial_manager.lock_or_recover("serial_manager");
                if let Err(e) = manager.send_packet(&packet) {
                    warn!("发送探测命令 'P' 失败: {}", e);
                }
            }
            (
                StatusCode::OK,
                Json(ApiResponse {
                    success: true,
                    message: format!("已连接到 {}", port_name),
                }),
            )
        }
        Ok(Err(e)) => {
            warn!("串口连接失败: {}", e);
            (
                StatusCode::SERVICE_UNAVAILABLE,
                Json(ApiResponse {
                    success: false,
                    message: format!("连接失败: {}", e),
                }),
            )
        }
        Err(e) => {
            warn!("串口连接任务异常: {}", e);
            (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(ApiResponse {
                    success: false,
                    message: format!("连接任务异常: {}", e),
                }),
            )
        }
    }
}

/// 断开串口
pub async fn disconnect_serial(
    State(state): State<Arc<AppState>>,
) -> (StatusCode, Json<ApiResponse>) {
    let mut manager = state.serial_manager.lock_or_recover("serial_manager");
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

/// 获取 BLE 设备列表
pub async fn get_ble_devices(State(state): State<Arc<AppState>>) -> Json<serde_json::Value> {
    let devices = state.ble_devices.lock_or_recover("ble_devices");
    let device_list: Vec<serde_json::Value> = devices
        .iter()
        .map(|d| {
            let mut json = serde_json::json!({
                "name": d.name,
                "mac": d.mac,
                "rssi": d.rssi
            });
            // wifi_mac 为可选项：仅车载 C6/S3 等设备会广播
            // 与 WebSocket 广播格式保持一致
            if let Some(ref wm) = d.wifi_mac {
                json["wifi_mac"] = serde_json::Value::String(wm.clone());
            }
            json
        })
        .collect();

    Json(serde_json::json!({
        "success": true,
        "devices": device_list
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::extract::State;

    /// 辅助函数：创建测试用 AppState
    fn create_test_state() -> Arc<AppState> {
        Arc::new(AppState::new())
    }

    /// 测试 CommandRequest 反序列化
    #[test]
    fn test_command_request_deserialize() {
        let json = r#"{"command":"W"}"#;
        let req: CommandRequest = serde_json::from_str(json).expect("CommandRequest 反序列化失败");
        assert_eq!(req.command, "W");
    }

    /// 测试 ConnectRequest 反序列化（含 baud_rate）
    #[test]
    fn test_connect_request_with_baud_rate() {
        let json = r#"{"port_name":"COM3","baud_rate":115200}"#;
        let req: ConnectRequest = serde_json::from_str(json).expect("ConnectRequest 反序列化失败");
        assert_eq!(req.port_name, "COM3");
        assert_eq!(req.baud_rate, Some(115200));
    }

    /// 测试 ConnectRequest 反序列化（无 baud_rate）
    #[test]
    fn test_connect_request_without_baud_rate() {
        let json = r#"{"port_name":"/dev/ttyUSB0"}"#;
        let req: ConnectRequest = serde_json::from_str(json).expect("ConnectRequest 反序列化失败");
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
        let json = serde_json::to_string(&resp).expect("ApiResponse 序列化失败");
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
            current_speed: 128,
            ws_clients: 0,
            uptime: 42,
            version: "1.2.0".to_string(),
            left_speed: 0.0,
            right_speed: 0.0,
            heading: 0.0,
            total_distance: 0.0,
            command_count: 0,
        };
        let json = serde_json::to_string(&resp).expect("StatusResponse 序列化失败");
        assert!(json.contains("\"serial_status\":\"未连接\""));
        assert!(json.contains("\"current_speed\":128"));
        assert!(json.contains("\"version\":\"1.2.0\""));
    }

    /// 测试超长命令处理：handle_command 应只取第一个字节，不 panic
    #[tokio::test]
    async fn test_handle_command_too_long() {
        let state = create_test_state();

        // 构造一个超长命令字符串（256 字节）
        let long_command = "W".repeat(256);
        let request = CommandRequest {
            command: long_command,
        };

        // 调用 handle_command（无串口连接时应返回 503）
        let (status, Json(resp)) = handle_command(State(state), Json(request)).await;

        // 无串口连接时发送失败，但不应 panic
        assert_eq!(
            status,
            StatusCode::SERVICE_UNAVAILABLE,
            "无串口连接时应返回 503 状态码"
        );
        assert!(!resp.success, "发送失败时 success 应为 false");
    }

    /// 测试特殊字符命令处理：包括空格、换行符、Unicode 等
    #[tokio::test]
    async fn test_handle_command_special_chars() {
        let state = create_test_state();

        // 测试空格命令（空格是有效的停车命令）
        let request = CommandRequest {
            command: " ".to_string(),
        };
        let (status, _) = handle_command(State(state.clone()), Json(request)).await;
        assert_eq!(
            status,
            StatusCode::SERVICE_UNAVAILABLE,
            "空格命令无串口时应返回 503"
        );

        // 测试换行符命令（不在合法单字符命令集中，应返回 400）
        let request = CommandRequest {
            command: "\n".to_string(),
        };
        let (status, _) = handle_command(State(state.clone()), Json(request)).await;
        assert_eq!(
            status,
            StatusCode::BAD_REQUEST,
            "换行符命令应返回 400"
        );

        // 测试 Unicode 字符命令（不在合法单字符命令集中，应返回 400）
        let request = CommandRequest {
            command: "你".to_string(),
        };
        let (status, _) = handle_command(State(state.clone()), Json(request)).await;
        assert_eq!(
            status,
            StatusCode::BAD_REQUEST,
            "Unicode 命令应返回 400"
        );

        // 测试空命令（应返回 400 Bad Request）
        let request = CommandRequest {
            command: "".to_string(),
        };
        let (status, Json(resp)) = handle_command(State(state), Json(request)).await;
        assert_eq!(
            status,
            StatusCode::BAD_REQUEST,
            "空命令应返回 400 Bad Request"
        );
        assert!(!resp.success, "空命令应返回 success=false");
        assert!(
            resp.message.contains("不能为空"),
            "空命令错误消息应包含'不能为空'"
        );
    }
}
