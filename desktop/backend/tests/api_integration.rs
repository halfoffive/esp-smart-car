/**
 * HTTP API 集成测试
 * 测试 Axum 路由端点的完整请求/响应流程
 */
use std::sync::atomic::Ordering;
use std::sync::Arc;

use axum::{
    body::Body,
    http::{Request, StatusCode},
    middleware,
    routing::{get, post},
    Router,
};
use http_body_util::BodyExt;
use tower::ServiceExt;

use esp_smart_car_backend::{
    api::{self},
    serial::{SerialConnectionState, SerialManager},
    websocket, AppState,
};

const TEST_TOKEN: &str = "test-token";

/// 创建测试用 AppState（认证禁用，避免测试需要携带 Token）
fn create_test_state() -> Arc<AppState> {
    Arc::new(AppState::new_test())
}

/// 创建启用认证且使用固定 Token 的测试状态。
///
/// 说明：`AppState::with_token(...)` 在 lib.rs 中为私有，因此这里先用
/// `AppState::new_test()` 构造一个认证禁用的状态，再直接设置公有的
/// `api_token` 字段，使认证测试完全确定、不受环境变量影响。
fn create_auth_state(token: &str) -> Arc<AppState> {
    let mut state = AppState::new_test();
    state.api_token = Some(Arc::from(token));
    Arc::new(state)
}

/// 创建测试用 Router
fn create_test_app(state: Arc<AppState>) -> Router {
    let api_routes = Router::new()
        .route("/api/command", post(api::handle_command))
        .route("/api/status", get(api::get_status))
        .route("/api/connect", post(api::connect_serial))
        .route("/api/disconnect", post(api::disconnect_serial))
        .route("/api/ports", get(api::list_ports))
        .route("/api/ble-devices", get(api::get_ble_devices))
        .route_layer(middleware::from_fn_with_state(
            state.clone(),
            api::auth_middleware,
        ));

    Router::new()
        .route("/ws", get(websocket::ws_handler))
        .merge(api_routes)
        .with_state(state)
}

/// 读取响应体并解析为 JSON Value
async fn read_body_json(response: axum::response::Response<Body>) -> (StatusCode, serde_json::Value) {
    let status = response.status();
    let bytes = response
        .into_body()
        .collect()
        .await
        .expect("读取响应体失败")
        .to_bytes();
    let json: serde_json::Value =
        serde_json::from_slice(&bytes).expect("解析响应 JSON 失败");
    (status, json)
}

/// 查找一个真实可用的串口（用于需要真实连接的成功路径测试）
async fn find_test_port() -> Option<String> {
    tokio::task::spawn_blocking(|| SerialManager::list_ports().into_iter().next())
        .await
        .ok()
        .flatten()
}

/// 测试 GET /api/status 返回 200，并验证 typed 字段结构
#[tokio::test]
async fn test_get_status() {
    let state = create_test_state();
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .uri("/api/status")
                .body(Body::empty())
                .expect("构建 status 请求失败"),
        )
        .await
        .expect("请求 /api/status 失败");

    let (status, json) = read_body_json(response).await;
    assert_eq!(status, StatusCode::OK);

    // StatusResponse 字段
    assert!(json["serial_status"].is_string());
    assert!(json["port_name"].is_null() || json["port_name"].is_string());
    assert!(json["baud_rate"].is_null() || json["baud_rate"].is_u64());
    assert!(json["frame_count"].is_u64());
    assert!(json["bytes_sent"].is_u64());
    assert!(json["current_speed"].is_u64());
    assert!(json["ws_clients"].is_u64());
    assert!(json["uptime"].is_u64());
    assert!(json["version"].is_string());
    assert!(json["left_speed"].is_number());
    assert!(json["right_speed"].is_number());
    assert!(json["heading"].is_number());
    assert!(json["total_distance"].is_number());
    assert!(json["command_count"].is_u64());

    // drive_mode 已从 StatusResponse 移除
    assert!(
        json.get("drive_mode").is_none(),
        "StatusResponse 不应再包含 drive_mode 字段"
    );
}

/// 测试 GET /api/ports 返回 typed PortsResponse
#[tokio::test]
async fn test_list_ports() {
    let state = create_test_state();
    let real_ports = tokio::task::spawn_blocking(SerialManager::list_ports)
        .await
        .expect("扫描串口失败");

    // 填充 available_ports 缓存，使 /api/ports 返回可预期的列表
    state
        .available_ports
        .lock()
        .expect("available_ports 锁中毒")
        .extend(real_ports.clone());

    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .uri("/api/ports")
                .body(Body::empty())
                .expect("构建 ports 请求失败"),
        )
        .await
        .expect("请求 /api/ports 失败");

    let (status, json) = read_body_json(response).await;
    assert_eq!(status, StatusCode::OK);

    assert_eq!(json["success"], true);
    assert!(json["ports"].is_array());
    let ports: Vec<String> =
        serde_json::from_value(json["ports"].clone()).expect("ports 字段反序列化失败");
    assert_eq!(ports, real_ports);
}

/// 测试 POST /api/command 无串口时返回 503，并验证错误消息
#[tokio::test]
async fn test_command_no_serial() {
    let state = create_test_state();
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .method("POST")
                .uri("/api/command")
                .header("content-type", "application/json")
                .body(Body::from(r#"{"command":"W"}"#))
                .expect("构建 command 请求失败"),
        )
        .await
        .expect("请求 /api/command 失败");

    let (status, json) = read_body_json(response).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
    assert_eq!(json["success"], false);
    let msg = json["message"].as_str().expect("message 应为字符串");
    assert!(
        msg.contains("发送失败") || msg.contains("未连接"),
        "错误消息应说明串口未连接：{msg}"
    );
}

/// 测试 S:<pwm> 速度命令在已连接真实串口时成功
#[tokio::test]
async fn test_command_speed_success() {
    let Some(port) = find_test_port().await else {
        panic!("未找到真实串口，无法测试 S: 速度命令成功路径");
    };

    let state = create_test_state();
    let app = create_test_app(state.clone());

    // 连接真实串口
    let connect_body = format!(r#"{{"port_name":"{port}"}}"#);
    let connect_resp = app
        .clone()
        .oneshot(
            Request::builder()
                .method("POST")
                .uri("/api/connect")
                .header("content-type", "application/json")
                .body(Body::from(connect_body))
                .expect("构建 connect 请求失败"),
        )
        .await
        .expect("请求 /api/connect 失败");

    let (status, json) = read_body_json(connect_resp).await;
    assert_eq!(
        status,
        StatusCode::OK,
        "连接真实串口应成功: {}",
        json["message"]
    );
    assert_eq!(json["success"], true);
    assert!(
        json["message"]
            .as_str()
            .unwrap()
            .contains(&format!("已连接到 {port}")),
        "连接成功消息应包含端口号"
    );

    // 发送速度命令
    let cmd_resp = app
        .clone()
        .oneshot(
            Request::builder()
                .method("POST")
                .uri("/api/command")
                .header("content-type", "application/json")
                .body(Body::from(r#"{"command":"S:128"}"#))
                .expect("构建 command 请求失败"),
        )
        .await
        .expect("请求 /api/command 失败");

    let (status, json) = read_body_json(cmd_resp).await;
    assert_eq!(status, StatusCode::OK, "设置速度应成功: {}", json["message"]);
    assert_eq!(json["success"], true);
    let msg = json["message"].as_str().expect("message 应为字符串");
    assert!(
        msg.contains("速度已设置为 128"),
        "成功消息应包含速度值：{msg}"
    );

    assert_eq!(
        state.current_speed.load(Ordering::Relaxed),
        128,
        "状态中的 current_speed 应更新为 128"
    );
}

/// 测试无效多字节命令返回 400
#[tokio::test]
async fn test_command_invalid_multi_byte() {
    let state = create_test_state();
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .method("POST")
                .uri("/api/command")
                .header("content-type", "application/json")
                .body(Body::from(r#"{"command":"WX"}"#))
                .expect("构建 command 请求失败"),
        )
        .await
        .expect("请求 /api/command 失败");

    let (status, json) = read_body_json(response).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    assert_eq!(json["success"], false);
    let msg = json["message"].as_str().expect("message 应为字符串");
    assert!(
        msg.contains("仅接受单字符命令") || msg.contains("S:<pwm>"),
        "错误消息应说明仅支持单字符命令：{msg}"
    );
}

/// 测试 POST /api/disconnect 返回 success
#[tokio::test]
async fn test_disconnect() {
    let state = create_test_state();
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .method("POST")
                .uri("/api/disconnect")
                .body(Body::empty())
                .expect("构建 disconnect 请求失败"),
        )
        .await
        .expect("请求 /api/disconnect 失败");

    let (status, json) = read_body_json(response).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(json["success"], true);
    let msg = json["message"].as_str().expect("message 应为字符串");
    assert!(msg.contains("已断开"), "成功消息应包含'已断开'：{msg}");
}

/// 测试已连接状态下调用 /api/disconnect 仍返回 success
#[tokio::test]
async fn test_disconnect_when_connected() {
    let state = create_test_state();
    {
        let mut manager = state.serial_manager.lock().expect("serial_manager 锁中毒");
        manager.state = SerialConnectionState::Connected {
            port_name: "TEST_PORT".to_string(),
            baud_rate: 921600,
        };
    }

    let app = create_test_app(state.clone());

    let response = app
        .oneshot(
            Request::builder()
                .method("POST")
                .uri("/api/disconnect")
                .body(Body::empty())
                .expect("构建 disconnect 请求失败"),
        )
        .await
        .expect("请求 /api/disconnect 失败");

    let (status, json) = read_body_json(response).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(json["success"], true);
    let msg = json["message"].as_str().expect("message 应为字符串");
    assert!(msg.contains("已断开"), "成功消息应包含'已断开'：{msg}");

    {
        let manager = state.serial_manager.lock().expect("serial_manager 锁中毒");
        assert!(
            matches!(manager.state, SerialConnectionState::Disconnected),
            "断开操作后状态应为 Disconnected"
        );
    }
}

/// 测试 POST /api/connect 无效端口返回 503，并验证错误消息
#[tokio::test]
async fn test_connect_invalid_port() {
    let state = create_test_state();
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .method("POST")
                .uri("/api/connect")
                .header("content-type", "application/json")
                .body(Body::from(r#"{"port_name":"NONEXISTENT"}"#))
                .expect("构建 connect 请求失败"),
        )
        .await
        .expect("请求 /api/connect 失败");

    let (status, json) = read_body_json(response).await;
    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
    assert_eq!(json["success"], false);
    let msg = json["message"].as_str().expect("message 应为字符串");
    assert!(msg.contains("连接失败"), "错误消息应包含'连接失败'：{msg}");
}

/// 测试 WebSocket 升级请求被识别
/// 注意：oneshot 模式下无法完成完整的 WebSocket 握手，
/// Axum 的 DefaultOnFailedUpgrade 会返回 426 Upgrade Required，
/// 表示升级请求已被识别但无法在测试环境中完成协议切换
#[tokio::test]
async fn test_ws_upgrade() {
    let state = create_test_state();
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .uri("/ws")
                .header("upgrade", "websocket")
                .header("connection", "Upgrade")
                .header("sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==")
                .header("sec-websocket-version", "13")
                .body(Body::empty())
                .expect("构建 WebSocket 升级请求失败"),
        )
        .await
        .expect("请求 /ws 升级失败");

    // oneshot 模式下，WebSocket 升级请求被识别但无法完成握手，
    // 返回 426 (Upgrade Required) 而非 101 (Switching Protocols)
    assert_eq!(response.status(), StatusCode::UPGRADE_REQUIRED);
}

/// 测试 GET /api/ble-devices 返回 200 且包含 devices 数组
#[tokio::test]
async fn test_get_ble_devices() {
    let state = create_test_state();
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .uri("/api/ble-devices")
                .body(Body::empty())
                .expect("构建 ble-devices 请求失败"),
        )
        .await
        .expect("请求 /api/ble-devices 失败");

    let (status, json) = read_body_json(response).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(json["success"], true);
    assert!(json["devices"].is_array());
}

/// 测试认证启用时未携带 Token 的 API 请求返回 401 JSON
#[tokio::test]
async fn test_auth_required() {
    let state = create_auth_state(TEST_TOKEN);
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .uri("/api/status")
                .body(Body::empty())
                .expect("构建 status 请求失败"),
        )
        .await
        .expect("请求 /api/status 失败");

    let (status, json) = read_body_json(response).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    assert_eq!(json["success"], false);
    let msg = json["message"].as_str().expect("message 应为字符串");
    assert!(msg.contains("Unauthorized"), "401 消息应包含 Unauthorized：{msg}");
}

/// 测试认证启用时携带正确 Token 可通过
#[tokio::test]
async fn test_auth_with_valid_token() {
    let state = create_auth_state(TEST_TOKEN);
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .uri("/api/status")
                .header("authorization", format!("Bearer {TEST_TOKEN}"))
                .body(Body::empty())
                .expect("构建 status 请求失败"),
        )
        .await
        .expect("请求 /api/status 失败");

    assert_eq!(response.status(), StatusCode::OK);
}

/// 测试携带错误 Token 时返回 401 JSON
#[tokio::test]
async fn test_auth_with_wrong_token() {
    let state = create_auth_state(TEST_TOKEN);
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .uri("/api/status")
                .header("authorization", "Bearer wrong-token")
                .body(Body::empty())
                .expect("构建 status 请求失败"),
        )
        .await
        .expect("请求 /api/status 失败");

    let (status, json) = read_body_json(response).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    assert_eq!(json["success"], false);
    let msg = json["message"].as_str().expect("message 应为字符串");
    assert!(msg.contains("Unauthorized"), "401 消息应包含 Unauthorized：{msg}");
}
