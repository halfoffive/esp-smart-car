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

use axum::{
    body::Body,
    extract::State,
    http::{Request, StatusCode},
    middleware::Next,
    response::{IntoResponse, Response},
    Json,
};
use serde::{Deserialize, Serialize};
use subtle::ConstantTimeEq;
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
    pub frame_count: u64,
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
    /// 波特率（可选，默认 3000000）
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

/// 串口列表响应（typed，避免裸 serde_json::Value）
#[derive(Debug, Serialize)]
pub struct PortsResponse {
    /// 是否成功
    pub success: bool,
    /// 可用串口列表
    pub ports: Vec<String>,
}

/// BLE 设备响应（typed，避免裸 serde_json::Value）
#[derive(Debug, Serialize)]
pub struct BleDevicesResponse {
    /// 是否成功
    pub success: bool,
    /// BLE 设备列表
    pub devices: Vec<BleDeviceDto>,
}

/// BLE 设备 DTO（序列化用）
#[derive(Debug, Serialize)]
pub struct BleDeviceDto {
    /// 设备名称
    pub name: String,
    /// BLE MAC 地址
    pub mac: String,
    /// WiFi MAC 地址（可选）
    pub wifi_mac: Option<String>,
    /// 信号强度
    pub rssi: i16,
}

/// 列出可用串口（使用 AppState 缓存的串口列表，避免每次请求都执行 spawn_blocking）
pub async fn list_ports(State(state): State<Arc<AppState>>) -> (StatusCode, Json<PortsResponse>) {
    // 使用 AppState 缓存的串口列表，避免每次请求都执行 spawn_blocking
    // available_ports 改为 std::sync::Mutex 后用 lock_or_recover 短时持锁复制
    let ports = state
        .available_ports
        .lock_or_recover("available_ports")
        .clone();

    (
        StatusCode::OK,
        Json(PortsResponse {
            success: true,
            ports,
        }),
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
                let state_clone = Arc::clone(&state);
                let send_result = tokio::task::spawn_blocking(move || {
                    let mut manager = state_clone.serial_manager.lock_or_panic("serial_manager");
                    manager.send_packet(&packet)
                })
                .await;

                return match send_result {
                    Ok(Ok(())) => {
                        state.current_speed.store(speed, Ordering::Relaxed);
                        state.log_command_forward(b'S');
                        info!("设置速度 PWM: {}", speed);
                        (
                            StatusCode::OK,
                            Json(ApiResponse {
                                success: true,
                                message: format!("速度已设置为 {}", speed),
                            }),
                        )
                    }
                    Ok(Err(e)) => {
                        warn!("发送速度命令失败: {}", e);
                        (
                            StatusCode::SERVICE_UNAVAILABLE,
                            Json(ApiResponse {
                                success: false,
                                message: format!("发送失败: {}", e),
                            }),
                        )
                    }
                    Err(e) => {
                        warn!("速度发送任务异常: {}", e);
                        (
                            StatusCode::INTERNAL_SERVER_ERROR,
                            Json(ApiResponse {
                                success: false,
                                message: format!("发送任务异常: {}", e),
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

    // 单字符命令：长度 > 1 且不是 S: 格式时拒绝（避免误用多字节字符串作为命令）
    if request.command.len() > 1 {
        warn!(
            "多字节命令不支持（仅接受单字符或 S:<pwm>）: {}",
            request.command
        );
        return (
            StatusCode::BAD_REQUEST,
            Json(ApiResponse {
                success: false,
                message: "仅接受单字符命令或 'S:<pwm>' 格式".to_string(),
            }),
        );
    }

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

    // 在 spawn_blocking 中持锁 + 发送，避免阻塞 async 运行时
    let state_clone = Arc::clone(&state);
    let send_result = tokio::task::spawn_blocking(move || {
        let mut manager = state_clone.serial_manager.lock_or_panic("serial_manager");
        manager.send_packet(&packet)
    })
    .await;

    match send_result {
        Ok(Ok(())) => {
            state.log_command_forward(cmd);
            info!("发送命令: {}", request.command);
            (
                StatusCode::OK,
                Json(ApiResponse {
                    success: true,
                    message: format!("命令 '{}' 已发送", request.command),
                }),
            )
        }
        Ok(Err(e)) => {
            warn!("发送命令失败: {}", e);
            (
                StatusCode::SERVICE_UNAVAILABLE,
                Json(ApiResponse {
                    success: false,
                    message: format!("发送失败: {}", e),
                }),
            )
        }
        Err(e) => {
            warn!("命令发送任务异常: {}", e);
            (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(ApiResponse {
                    success: false,
                    message: format!("发送任务异常: {}", e),
                }),
            )
        }
    }
}

/// 获取系统状态
pub async fn get_status(State(state): State<Arc<AppState>>) -> (StatusCode, Json<StatusResponse>) {
    // 锁策略说明：
    // - `serial_manager` 使用 `lock_or_panic`：串口连接状态属于关键状态，中毒意味着
    //   数据可能已损坏，应快速失败而非继续对外暴露不一致的连接信息。
    // - `ws_manager` / `odometry` 使用 `lock_or_recover`：这两者仅用于状态展示（客户端
    //   计数、测速读数），中毒后丢失一次更新不影响协议正确性，恢复后服务可继续运行。
    // 逐把加锁，复制数据后立即释放，减少锁争用
    let (serial_status, port_name, baud_rate, frame_count, bytes_sent, command_count) = {
        let manager = state.serial_manager.lock_or_panic("serial_manager");
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
    let port_name = request.port_name.trim().to_string();
    if port_name.is_empty() {
        return (
            StatusCode::BAD_REQUEST,
            Json(ApiResponse {
                success: false,
                message: "端口名不能为空".to_string(),
            }),
        );
    }

    const MIN_BAUD: u32 = 1200;
    const MAX_BAUD: u32 = 12_000_000;
    let baud_rate = request.baud_rate.unwrap_or(DEFAULT_BAUD_RATE);
    if baud_rate < MIN_BAUD || baud_rate > MAX_BAUD {
        return (
            StatusCode::BAD_REQUEST,
            Json(ApiResponse {
                success: false,
                message: format!(
                    "波特率必须在 {}-{} 之间",
                    MIN_BAUD, MAX_BAUD
                ),
            }),
        );
    }

    let state_clone = Arc::clone(&state);
    let port_name_clone = port_name.clone();
    let connect_result = tokio::task::spawn_blocking(move || {
        let mut manager = state_clone.serial_manager.lock_or_panic("serial_manager");
        manager.disconnect();
        manager
            .connect(&port_name_clone, baud_rate)
            .map_err(|e| e.to_string())
    })
    .await;

    // 根据结果更新状态
    match connect_result {
        Ok(Ok(())) => {
            info!("串口连接成功: {} @ {}", port_name, baud_rate);

            // 连接成功后主动发送 'P' 探测命令，触发 Dongle 立即上报链路状态，
            // 避免前端等待最多 5 秒的周期上报。
            // 注意：此探测在单独的 spawn_blocking 中执行，不影响连接响应。
            // 添加 3 秒延迟：ESP32-C6 在串口打开时可能触发 DTR 复位重启，
            // 需等待设备完成启动后再发送探测命令，否则命令会丢失。
            let probe_state = Arc::clone(&state);
            let probe_port_name = port_name.clone();
            tokio::spawn(async move {
                tokio::time::sleep(tokio::time::Duration::from_secs(3)).await;
                let seq = probe_state.packet_seq.fetch_add(1, Ordering::Relaxed);
                let result = tokio::task::spawn_blocking(move || {
                    let mut manager = probe_state.serial_manager.lock_or_panic("serial_manager");
                    manager.send_packet(&crate::websocket::build_wireless_packet(11, 0, 0, seq))
                })
                .await;

                match result {
                    Ok(Ok(())) => {
                        info!("已发送链路探测命令到 {}", probe_port_name);
                    }
                    Ok(Err(e)) => {
                        warn!("发送链路探测命令失败 {}: {}", probe_port_name, e);
                    }
                    Err(e) => {
                        warn!("链路探测任务异常 {}: {}", probe_port_name, e);
                    }
                }
            });

            (
                StatusCode::OK,
                Json(ApiResponse {
                    success: true,
                    message: format!("已连接到 {}", port_name),
                }),
            )
        }
        Ok(Err(e)) => {
            warn!("串口连接或探测失败: {}", e);
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
    // 处理 JoinError：spawn_blocking 任务本身可能因 panic 失败，不能静默 .ok()
    let disconnect_result = tokio::task::spawn_blocking(move || {
        let mut manager = state.serial_manager.lock_or_panic("serial_manager");
        manager.disconnect();
    })
    .await;

    match disconnect_result {
        Ok(()) => {
            info!("串口已断开");
            (
                StatusCode::OK,
                Json(ApiResponse {
                    success: true,
                    message: "串口已断开".to_string(),
                }),
            )
        }
        Err(e) => {
            warn!("串口断开任务异常: {}", e);
            (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(ApiResponse {
                    success: false,
                    message: format!("断开任务异常: {}", e),
                }),
            )
        }
    }
}

/// 获取 BLE 设备列表
pub async fn get_ble_devices(State(state): State<Arc<AppState>>) -> Json<BleDevicesResponse> {
    let devices = state.ble_devices.lock_or_recover("ble_devices");
    let device_list: Vec<BleDeviceDto> = devices
        .iter()
        .map(|d| BleDeviceDto {
            name: d.name.clone(),
            mac: d.mac.clone(),
            // wifi_mac 为可选项：仅车载 C6/S3 等设备会广播
            // 与 WebSocket 广播格式保持一致
            wifi_mac: d.wifi_mac.clone(),
            rssi: d.rssi,
        })
        .collect();

    Json(BleDevicesResponse {
        success: true,
        devices: device_list,
    })
}

/// API 认证中间件
/// 当 `AppState.api_token` 为 Some 时，要求请求头携带 `Authorization: Bearer <token>`
/// 认证禁用（测试环境）时直接放行
pub async fn auth_middleware(
    State(state): State<Arc<AppState>>,
    request: Request<Body>,
    next: Next,
) -> Response {
    // 一次性处理 None 分支：认证禁用时直接放行，避免后续 unwrap_or("") 不可达分支
    let Some(expected) = state.api_token.as_deref() else {
        return next.run(request).await;
    };

    let provided = request
        .headers()
        .get("authorization")
        .and_then(|h| h.to_str().ok())
        .and_then(|s| s.strip_prefix("Bearer "))
        .unwrap_or("");

    // 恒定时间比较，避免时序侧信道攻击（SubTask 1.1）
    // ct_eq 返回 Choice，用 bool::from 转换；长度不同时 ct_eq 返回 false
    if !bool::from(provided.as_bytes().ct_eq(expected.as_bytes())) {
        warn!("API 认证失败：Authorization 头中的 token 不匹配");
        return (
            StatusCode::UNAUTHORIZED,
            Json(ApiResponse {
                success: false,
                message: "Unauthorized".into(),
            }),
        )
            .into_response();
    }

    next.run(request).await
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::extract::State;

    /// 辅助函数：创建测试用 AppState
    fn create_test_state() -> Arc<AppState> {
        Arc::new(AppState::new_test())
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
        let json = serde_json::to_string(&resp).expect("StatusResponse 序列化失败");
        assert!(json.contains("\"serial_status\":\"未连接\""));
        assert!(json.contains("\"current_speed\":5"));
        assert!(json.contains("\"version\":\"1.2.0\""));
    }

    /// 测试超长命令处理：handle_command 应拒绝多字节命令，返回 400 而非尝试发送
    #[tokio::test]
    async fn test_handle_command_too_long() {
        let state = create_test_state();

        // 构造一个超长命令字符串（256 字节）
        let long_command = "W".repeat(256);
        let request = CommandRequest {
            command: long_command,
        };

        // 多字节命令应被拒绝（SubTask 1.5），返回 400
        let (status, Json(resp)) = handle_command(State(state), Json(request)).await;

        assert_eq!(
            status,
            StatusCode::BAD_REQUEST,
            "多字节命令应返回 400 状态码"
        );
        assert!(!resp.success, "拒绝时 success 应为 false");
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
        assert_eq!(status, StatusCode::BAD_REQUEST, "换行符命令应返回 400");

        // 测试 Unicode 字符命令（不在合法单字符命令集中，应返回 400）
        let request = CommandRequest {
            command: "你".to_string(),
        };
        let (status, _) = handle_command(State(state.clone()), Json(request)).await;
        assert_eq!(status, StatusCode::BAD_REQUEST, "Unicode 命令应返回 400");

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

    /// 测试无效速度命令返回 400
    #[tokio::test]
    async fn test_handle_command_invalid_speed() {
        let state = create_test_state();
        let request = CommandRequest {
            command: "S:256".to_string(),
        };
        let (status, Json(resp)) = handle_command(State(state), Json(request)).await;
        assert_eq!(status, StatusCode::BAD_REQUEST);
        assert!(!resp.success);
        assert!(resp.message.contains("PWM 值无效"));
    }
}
