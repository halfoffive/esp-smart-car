# 桌面端后端 - Knowledge Base

**Location:** `desktop/backend/`
**Language:** Rust
**Framework:** Axum (Web) + Tokio (async) + serialport (USB)

## Structure

```
desktop/backend/
├── Cargo.toml             # 依赖配置
├── .env                   # 环境变量
└── src/
    ├── lib.rs           # 应用状态定义
    ├── main.rs            # 主程序（HTTP服务器 + WebSocket）
    ├── serial.rs          # 串口通信管理
    ├── websocket.rs       # WebSocket 处理
    └── api.rs             # HTTP API 端点
```

## Where to Look

| Task | Location | Notes |
|------|----------|-------|
| 修改 WebSocket 逻辑 | `src/websocket.rs` | 视频传输 + 命令转发 |
| 修改串口通信 | `src/serial.rs` | 921600 波特率 |
| 添加 API 端点 | `src/api.rs` | RESTful API |
| 修改服务器配置 | `src/main.rs` | 路由 + 状态管理 |
| 修改静态文件嵌入 | `src/main.rs` | `rust-embed` `Assets` 结构体 |
| 添加依赖 | `Cargo.toml` | Rust 包管理 |

## Code Map

| Symbol | Type | Location | Role |
|--------|------|----------|------|
| `AppState` | struct | `lib.rs` | 全局共享状态 |
| `Assets` | struct | `main.rs` | `rust-embed` 静态文件嵌入（`frontend/dist`） |
| `SerialManager` | struct | `serial.rs` | 串口连接管理 |
| `WebSocketManager` | struct | `websocket.rs` | 客户端管理 |
| `SerialConnectionState` | enum | `serial.rs` | 连接状态 |
| `BleDevice` | struct | `lib.rs` | BLE 设备信息 |
| `CommandRequest` | struct | `api.rs` | API 请求体 |
| `StatusResponse` | struct | `api.rs` | API 响应体；`current_speed` 表示 0-255 PWM |

## Conventions

- **Tokio async**：所有 I/O 操作使用 async/await
- **Arc + Mutex**：共享状态通过 `Arc<Mutex<T>>` 传递
- **Error handling**：使用 `anyhow::Result` 进行错误传播
- **Tracing**：结构化日志，使用 `tracing` crate
- **模块化**：serial、websocket、api 分离为独立模块
- **不可变引用**：优先使用 `&` 而非 `&mut`

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/ws` | WebSocket 连接 |
| POST | `/api/command` | 发送控制命令 |
| GET | `/api/ports` | 获取可用串口列表 |
| GET | `/api/status` | 获取系统状态 |
| POST | `/api/connect` | 连接串口 |
| POST | `/api/disconnect` | 断开串口 |
| GET | `/api/ble-devices` | 获取 BLE 设备列表 |

## Anti-Patterns

- **禁止使用 `unwrap`**：所有错误必须显式处理
- **阻塞 I/O 隔离**：串口等阻塞 I/O 通过 `tokio::task::spawn_blocking` 隔离，避免阻塞 async 运行时
- **禁止全局状态**：使用 `AppState` 传递共享状态
- **禁止裸指针**：使用安全 Rust 抽象

## Commands

```bash
# 开发
# 注意：build.rs 不再自动运行 bun install/build，首次或前端源码变更后需先手动构建前端
cd ../frontend && bun install && bun run build && cd ../backend
cargo build          # 编译（将已构建的 frontend/dist 嵌入二进制）
cargo run            # 运行
cargo test           # 运行测试
cargo check          # 快速检查（不编译）

# 生产
cargo build --release  # 优化编译
```

## Notes

- **前端嵌入**：前端资源通过 `rust-embed` 编译进二进制，`exe` 可在任意位置运行，无需 `frontend/dist` 目录伴随
- **配置文件**：`.env` 文件是可选的。`exe` 移动到其他位置时，使用内置默认值（`RUST_LOG=info`）；仅在开发目录中放置 `.env` 可覆盖默认配置
- **端口**：HTTP 服务器监听 8080，WebSocket 在 `/ws`
- **串口**：默认 921600 波特率，支持动态连接/断开
- **视频帧**：通过 WebSocket 发送 Base64 编码的 JPEG
- **速度语义**：`StatusResponse.current_speed` 与共享状态 `current_speed` 均为 0-255 PWM
- **串口协议**：PC → receiver_dongle 使用 8 字节二进制 `WirelessPacket`（含 checksum），不再使用单字符命令
- **串口列表**：`GET /api/ports` 返回 `PortsResponse { success, ports: Vec<String> }`，列表同时通过 WebSocket `port_list` 消息实时推送
- **认证中间件**：REST 端点要求 `Authorization: Bearer <token>`，WebSocket 握手要求 URL 查询参数 `?token=<token>`；Token 使用 `subtle::ConstantTimeEq` 恒定时间比较，失败返回 JSON 格式 401。未设置 `API_TOKEN` 时（或 `DISABLE_AUTH=true`）使用默认 Token `esp-smart-car`（仅本地开发）
- **WebSocket 广播**：`video_task` 使用 `tokio::sync::Notify` 事件驱动唤醒；`WebSocketManager` 客户端 ID 与连接数使用原子计数器维护；视频帧按 hash 去重，仅在新帧时广播
- **心跳**：30 秒间隔，90 秒超时，防止连接超时
- **CORS**：前端开发时启用跨域支持

## 近期修复记录

### 2026-06-20 - Karpathy 审计修复

**背景**: 完成 Karpathy 指南漏洞审计，报告见 `docs/karpathy_vulnerability_report.md`，共发现并修复 52 项问题。

**本模块修复**:

- **P0**:
  - REST/WebSocket 认证 — `api.rs` / `websocket.rs` 增加统一认证中间件，未携带凭证调用 `/api/command`、`/api/connect`、`/ws` 返回 401/403 或立即断开
- **P1**:
  - 串口写操作 `spawn_blocking` — `websocket.rs` / `api.rs` 将 `send_packet` / `send_bytes` 包进 `tokio::task::spawn_blocking`，避免阻塞 async 运行时
  - 心跳按客户端持有 — `websocket.rs` / `lib.rs` 将全局 `last_heartbeat` 改为每个 WebSocket 连接独立持有
  - 串口 `Ok(0)` 断开检测 — `serial.rs` `read_next` / `resync_stream` 将 `Ok(0)` 视为 EOF，立即返回错误
  - 串口重连旧句柄释放 — `serial.rs` / `api.rs` 优化 disconnect → connect 时旧 `SerialPort` 的 Drop 时序，降低 Windows COM 口独占失败概率
  - TLS/加密配置路径 — `main.rs` / `Cargo.toml` 增加 TLS 证书路径与 `wss://`/`https://` 启动选项
- **P2**:
  - JSON 解析健壮性 — `serial.rs` 先 `serde_json::from_str` 再判断 `t` 字段，避免对合法空格/字段顺序敏感
  - 串口任务退避溢出 — `main.rs` 限制移位量，避免 65 次连续失败后 panic
  - 全局 Mutex 中毒处理策略 — `lib.rs` `MutexExt::lock_or_recover` 对关键状态返回 `Result`
  - `connect_serial` 原子性 — `api.rs` 将 disconnect + connect 整体放入 `spawn_blocking`
  - 后端输入校验 — `websocket.rs` `speed` / `drive_mode` 非法输入返回 `error` 消息
  - BLE 列表过期清空 — `websocket.rs` 即使为空也广播 `{"type":"ble_devices","devices":[]}`
  - 视频帧上限对齐 — `serial.rs` 帧大小上限从 256KB 改为接收器缓冲区 32KB
  - `command_count` 准确性 — `serial.rs` 仅在控制/速度/模式命令时递增
- **P3**:
  - `static_handler` 移除 `expect` — `main.rs` 改为保守 500 响应
  - `tokio` 特性精简 — `Cargo.toml` 从 `full` 改为显式特性列表
  - BLE 集成测试补充 — `tests/api_integration.rs` 挂载 `/api/ble-devices` 并补充基础 GET 测试
  - 注释/冗余清理 — 修正 ESP-NOW 遗留描述、移除原始 `video_frame` 冗余字段

**验证**: `cargo check` / `cargo clippy` / `cargo test` 因当前 Windows 环境 Rust 1.96.0 的 `std::process::Command::output` 返回 `Os { code: 0 }` 暂无法运行，与本模块代码无关。
