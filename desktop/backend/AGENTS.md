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
cargo build          # 编译
cargo run            # 运行（带热重载）
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
- **串口协议**：PC → receiver_dongle 使用 12 字节二进制 `WirelessPacket`（含 checksum），不再使用单字符命令
- **心跳**：30 秒间隔，防止连接超时
- **CORS**：前端开发时启用跨域支持
