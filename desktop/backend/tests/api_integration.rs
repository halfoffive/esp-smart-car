/**
 * HTTP API 集成测试
 * 测试 Axum 路由端点的完整请求/响应流程
 */
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

use esp_smart_car_backend::{api, websocket, AppState};

/// 创建测试用 AppState（认证禁用，避免测试需要携带 Token）
fn create_test_state() -> Arc<AppState> {
    Arc::new(AppState::new_test())
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

/// 测试 GET /api/status 返回 200
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

    assert_eq!(response.status(), StatusCode::OK);

    let body = response.into_body();
    let bytes = body.collect().await.expect("读取响应体失败").to_bytes();
    let json: serde_json::Value =
        serde_json::from_slice(&bytes).expect("解析 status 响应 JSON 失败");

    assert!(json["serial_status"].is_string());
    assert!(json["current_speed"].is_number());
    assert!(json["version"].is_string());
}

/// 测试 POST /api/command 无串口时返回 503
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

    assert_eq!(response.status(), StatusCode::SERVICE_UNAVAILABLE);
}

/// 测试 POST /api/disconnect 返回 200
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

    assert_eq!(response.status(), StatusCode::OK);

    let body = response.into_body();
    let bytes = body.collect().await.expect("读取响应体失败").to_bytes();
    let json: serde_json::Value =
        serde_json::from_slice(&bytes).expect("解析 disconnect 响应 JSON 失败");

    assert_eq!(json["success"], true);
}

/// 测试 POST /api/connect 无效端口返回 503
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

    assert_eq!(response.status(), StatusCode::SERVICE_UNAVAILABLE);
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

    assert_eq!(response.status(), StatusCode::OK);

    let body = response.into_body();
    let bytes = body.collect().await.expect("读取响应体失败").to_bytes();
    let json: serde_json::Value =
        serde_json::from_slice(&bytes).expect("解析 ble-devices 响应 JSON 失败");

    assert_eq!(json["success"], true);
    assert!(json["devices"].is_array());
}

/// 测试认证启用时未携带 Token 的 API 请求返回 401
#[tokio::test]
async fn test_auth_required() {
    let state = Arc::new(AppState::new());
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

    assert_eq!(response.status(), StatusCode::UNAUTHORIZED);
}

/// 测试认证启用时携带正确 Token 可通过
#[tokio::test]
async fn test_auth_with_valid_token() {
    let state = Arc::new(AppState::new());
    let token = state.api_token.clone().expect("应自动生成 API Token");
    let app = create_test_app(state);

    let response = app
        .oneshot(
            Request::builder()
                .uri("/api/status")
                .header("authorization", format!("Bearer {}", token))
                .body(Body::empty())
                .expect("构建 status 请求失败"),
        )
        .await
        .expect("请求 /api/status 失败");

    assert_eq!(response.status(), StatusCode::OK);
}
