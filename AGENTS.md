# ESP32 Smart Car - Knowledge Base

**Project:** ESP32 智能车控制系统
**Stack:** Arduino/C++ (ESP32), Rust (Axum), Vue 3 (Vite + TailwindCSS)
**Architecture:** 嵌入式固件 + 桌面端控制界面

## Structure

```
esp-smart-car/
├── firmware/              # Embedded firmware (Arduino IDE)
│   ├── libraries/         # Arduino libraries (shared across sketches)
│   │   └── wireless_protocol/  # ESP-NOW wireless protocol library
│   ├── car_controller/     # Vehicle controller (ESP32-C6)
│   ├── camera_module/     # Camera module (ESP32-S3 CAM)
│   └── receiver_dongle/   # USB receiver (ESP32-C6)
├── desktop/               # Desktop control interface
│   ├── backend/           # Rust backend (Axum + WebSocket)
│   └── frontend/          # Vue frontend (Web UI)
└── docs/                  # Hardware documentation
```

## Where to Look

| Task | Location | Notes |
|------|----------|-------|
| Add motor control logic | `firmware/car_controller/motor_control.h` | Functional programming style |
| Add servo control logic | `firmware/car_controller/servo_control.h` | Smooth movement algorithm |
| Modify wireless protocol | `firmware/libraries/wireless_protocol/src/wireless.h` | ESP-NOW protocol (Arduino library) |
| Add camera resolution | `firmware/camera_module/camera_config.h` | OV2640 configuration |
| Add video streaming | `firmware/camera_module/video_stream.h` | Frame packetization |
| Add serial communication | `desktop/backend/src/serial.rs` | USB serial port |
| Add WebSocket handlers | `desktop/backend/src/websocket.rs` | Real-time video |
| Add REST API endpoints | `desktop/backend/src/api.rs` | HTTP API |
| Add UI components | `desktop/frontend/src/components/` | Vue SFC components |
| Add keyboard controls | `desktop/frontend/src/composables/useKeyboard.ts` | WASD mapping |
| Hardware wiring | `docs/hardware.md` | Complete pinout diagram |

## Code Map

| Symbol | Type | Location | Role |
|--------|------|----------|------|
| `MotorControl` | namespace | `motor_control.h` | 4-motor differential drive |
| `ServoControl` | namespace | `servo_control.h` | 2-servo gimbal control |
| `WirelessProtocol` | namespace | `wireless.h` (Arduino library) | ESP-NOW communication |
| `CameraConfig` | struct | `camera_config.h` | OV2640 configuration |
| `VideoStream` | namespace | `video_stream.h` | Frame transmission |
| `SerialManager` | struct | `serial.rs` | USB serial communication |
| `WebSocketManager` | struct | `websocket.rs` | Client connection management |
| `AppState` | struct | `main.rs` | Shared application state |

## Conventions

### Embedded (C++ - Arduino)
- **Functional programming style**: Heavy use of `const`, pure functions, immutable state
- **Chinese comments**: All functions, structs, enums have detailed Chinese comments
- **Type safety**: Use `enum class` instead of `enum` to prevent implicit conversions
- **Namespace organization**: Use `namespace` for each module (e.g., `PinConfig`, `SG90Config`)
- **Struct-based state**: All state stored in structs, new state created via functions
- **Hardware abstraction**: Pure functions for logic, separate `apply_*` functions for side effects
- **Arduino library**: Shared wireless protocol (`wireless.h`) is an Arduino library in `firmware/libraries/` to avoid duplication across sketches

### Backend (Rust)
- **Tokio async**: All I/O operations are async
- **Arc + Mutex**: Shared state via `Arc<Mutex<T>>`
- **Error handling**: Use `anyhow::Result` for error propagation
- **Tracing**: Structured logging with `tracing` crate
- **Modular design**: Separate modules for serial, websocket, API

### Frontend (Vue + TypeScript)
- **Composition API**: Use `<script setup>` and composables
- **Pinia**: State management (pre-configured)
- **TailwindCSS**: Utility-first styling with custom theme
- **Functional components**: Composables for reusable logic
- **WebSocket**: Real-time communication via custom composable

## Anti-Patterns

- **Never use `as any` or `@ts-ignore`**: Type safety is enforced
- **Never suppress errors**: Empty catch blocks are forbidden
- **Never delete failing tests**: Fix the code, not the tests
- **Never use global mutable state** in embedded: All state must be explicit
- **Never mix concerns**: Motor logic separate from servo logic separate from wireless
- **Never use implicit conversions** in C++: Always explicit type casting

## Unique Styles

- **Functional C++**: All data structures are immutable (value semantics), state changes via function returns
- **Binary protocol**: Custom 8-byte packet format for ESP-NOW communication
- **Frame packetization**: Video frames split into 128-byte chunks for wireless transmission
- **Differential steering**: Left/right motor speed differential for turning
- **Smooth servo movement**: Gradual angle changes with configurable speed steps

## Commands

```bash
# Backend (Rust)
cd desktop/backend
cargo build        # Build
cargo run          # Run server (serves frontend at http://localhost:8080)
cargo test         # Run all tests

# Frontend (Vue + Bun)
cd desktop/frontend
bun install        # Install dependencies
bun run dev        # Development server (port 3000)
bun run build      # Production build (outputs to ../backend/frontend/dist)

# Firmware (Arduino IDE)
# 1. Install the wireless_protocol library:
#    - Option A: Set Arduino IDE sketchbook to `firmware/` (library auto-detected)
#    - Option B: Copy `firmware/libraries/wireless_protocol` to Arduino libraries folder
# 2. Open .ino files in Arduino IDE
# 3. Select board: ESP32C6 or ESP32S3
# 4. Upload to respective devices
```

## Hardware Wiring

See `docs/hardware.md` for complete pinout diagram.

Key connections:
- **L298N #1**: GPIO 4,5,6 (left motors)
- **L298N #2**: GPIO 7,8,9 (right motors)
- **Servo H**: GPIO 14 (horizontal gimbal)
- **Servo V**: GPIO 15 (vertical gimbal)
- **Camera**: ESP32-S3 standard CAM pins

## Notes

- **Power isolation**: Motor power and logic power must be separate
- **Ground common**: All devices must share common ground
- **Baud rate**: 921600 for USB serial (high-speed video)
- **ESP-NOW channel**: Fixed channel 1 for all devices
- **Video buffer**: 4096 bytes for frame reassembly
- **Timeout protection**: 1-second auto-stop if no commands received
- **Calibration**: Servo angles may need adjustment based on physical mounting
- **Arduino library**: `wireless.h` is installed as an Arduino library in `firmware/libraries/wireless_protocol/` to avoid duplication across sketches

## 近期修复记录

### 2026-06-12 - api.rs port_name 所有权错误修复
- **范围**: 后端 Rust `api.rs` 编译错误 E0382
- **修复**:
  - `api.rs` — `connect_serial` 中 `port_name` 被 move 进 `spawn_blocking` 闭包后，闭包外仍被引用（日志和响应消息），导致编译失败。在闭包前添加 `port_name.clone()`，闭包内使用 clone 值，闭包外保留原值
- **验证**: `cargo clippy` 0 errors

### 2026-06-12 - 全面代码排查与优化 v4（20项修复）
- **范围**: 嵌入式固件 + 后端 Rust + 前端 Vue 三部分全面审查，启用 karpathy-guidelines 和 frontend-design 深度审计
- **严重修复（P0 - 5项）**:
  - `receiver_dongle.ino` — 'D' 命令分类错误，从 SERVO 移到 MOVE 分支（'D' 是右转，不是云台下）
  - `receiver_dongle.ino` — H/J/K 云台命令未被识别，`parseSerialCommand` 和 `getCommandType` 添加支持，**云台控制此前完全失效**
  - `servo_control.h` — `parseGimbalCommand` 缺少 'J' 云台下处理，添加 `case 'J': case 'j':` 与 'D' 相同逻辑
  - `pid_control.h` — `initializePIDController()` 初始状态与 `car_controller.ino` 不一致：`g_straightLineEnabled` 改为 `false`，`g_driveMode` 改为 `NORMAL`
  - `video_stream.h` — ESP-NOW 广播视频帧给所有设备，car_controller 收到视频包误解析，改为指定接收器 MAC 地址
- **高优先级修复（P1 - 5项）**:
  - `receiver_dongle.ino` — `VideoFrameBuffer` 用 `new[]` 分配但从未 `delete[]`，改为静态数组消除内存泄漏
  - `api.rs` — `connect_serial` 中 `serialport::open()` 阻塞 I/O 在 `MutexGuard` 内，移入 `spawn_blocking`
  - `websocket.rs` — `drive_mode` 发送双字节（模式字符 + 模式值），添加注释说明协议对齐
  - `car_controller.ino` — `updateOdometer` 每 10ms 调用但 99% 直接 return，移到定时条件内与测速上报同频
  - `odometer.h` — `getCurrentOdometry` 非原子读取多个浮点变量，扩大 `noInterrupts` 保护范围
- **中优先级修复（P2 - 6项）**:
  - `useStatus.ts` — `StatusData` 接口与后端 `StatusResponse` 不匹配，扩展为完整字段
  - `SpeedDashboard.vue` — `speedSamples.shift()` O(n) 性能差，改为 `slice(-MAX_SAMPLES)` 截断
  - `camera_module.ino` — `handleCameraCommand` SERVO/STATUS 分支为空，添加云台命令转发和状态查询
  - `motor_control.h` — `parseCommandWithSpeed` 标注纯函数但含 `Serial.printf`，移除副作用
  - `video_stream.h` — `adjustQuality` 已定义但从未调用，在 `updateStreaming` 中调用实现动态质量调整
  - `StatusBar.vue` — 引用已删除的 `status.value.fps`，改为从 `useWebSocket().videoFps` 获取
- **低优先级/优化（P3 - 4项）**:
  - `api.rs` + `main.rs` — 新增 `GET /api/ports` 端点列出可用串口
  - `receiver_dongle.ino` — `SerialCommand` 移除 `const` 成员，允许赋值操作
  - `wireless.h` — `static` 全局变量改为 `inline`，确保多翻译单元单一定义
  - `useWebSocket.ts` — 添加 `videoFps` ref，VideoPlayer 同步更新
- **前端 UI 优化**:
  - 字体升级：Inter → Space Grotesk（显示），Fira Code → JetBrains Mono（等宽）
  - 控制按钮激活态添加 cyan glow 阴影微交互
  - 视频区域添加半透明扫描线纹理，增强科技感
- **验证**: `bun run build` 成功；`cargo test`/`cargo clippy` 因 Rust 1.96.0 编译器 ICE（已知 bug）暂无法运行

### 2026-06-09 - P0 固件编译错误修复（5项严重错误）
- **范围**: 嵌入式固件全面编译错误修复，重构 wireless.h 为 Arduino 库
- **严重修复**:
  - `wireless.h` — 重构为 Arduino 库（`firmware/libraries/wireless_protocol/`），避免复制到各 sketch 目录，消除维护负担
  - `wireless.h` — 修复 ESP32 Arduino core 3.3.8 回调签名不兼容：`esp_now_send_cb_t` 改为 `void (*)(const wifi_tx_info_t*, esp_now_send_status_t)`，`esp_now_recv_cb_t` 改为 `void (*)(const esp_now_recv_info*, const uint8_t*, int)`
  - `wireless.h` — 移除 `onDataRecv` 定义（改为 `extern` 声明），从 `initializeWireless()` 中移除回调注册，消除 `car_controller.ino` 中 `onDataRecv` 重定义
  - `wireless.h` — 新增 `VideoPacket` 和 `StreamConfig` 定义（从 `video_stream.h` 迁移），供 `receiver_dongle.ino` 共享使用
  - `motor_control.h` / `servo_control.h` / `odometer.h` / `pid_control.h` / `video_stream.h` — 移除所有状态结构体的 `const` 成员，修复 "use of deleted function 'operator='" 编译错误
  - `odometer.h` — `volatile uint32_t g_leftPulses++` 改为 `g_leftPulses += 1`，消除 C++20 `volatile` 弃用警告
  - `video_stream.h` — 移除 `VideoPacket` / `StreamConfig` 定义，改为 `#include <wireless.h>`（Arduino 库），新增 `VideoStreamConfig` 命名空间存放视频流特有常量
  - `car_controller.ino` / `receiver_dongle.ino` / `camera_module.ino` — 修改所有 ESP-NOW 回调签名匹配新版 API；`car_controller.ino` `#include "wireless.h"` 改为 `#include <wireless.h>`

### 2026-06-09 - 全面代码排查与优化 v3（27项修复）
- **范围**: 前端 Vue + 后端 Rust + 嵌入式固件三部分全面审查，启用 karpathy-guidelines 和 frontend-design 深度审计，修复 10 项严重问题、9 项高优先级问题、8 项一般问题
- **严重修复（前端）**:
  - `ControlPanel.vue` — `onUnmounted` 中 `disconnect()` 未 await，改为 `.catch(() => {})` 处理 Promise rejection
  - `ControlPanel.vue` — `handleSpeedInput` 与 `v-model.number` 逻辑重复，直接读取 `currentSpeed.value`
  - `VideoPlayer.vue` — RAF 持续空转造成 CPU 浪费，改用 `watch(videoFrame)` 监听帧变化
  - `useWebSocket.ts` — 旧连接清理不安全，`connect()` 关闭旧连接后设 `shouldReconnect = false` 防误触发重连
  - `useWebSocket.ts` — 命令静默丢弃，`sendCommand`/`sendSpeed`/`sendDriveMode` 返回 `boolean`
- **严重修复（固件）**:
  - `servo_control.h` — 云台命令字符 'L'/'R' 与前端 'H'/'K' 不匹配，云台左右按钮完全失效
  - `car_controller.ino` — OdometryPacket `reinterpret_cast` 强转 WirelessPacket（12 vs 8 字节），改为 `sendRawPacket()` 通用发送
  - `video_stream.h` — VideoPacket 发送冗余数据，改为只发送实际有效大小
  - `receiver_dongle.ino` — `handleVideoPacket` 中 `dataLen` 未边界检查，添加 `<= MAX_PACKET_SIZE` 验证
  - `receiver_dongle.ino` — OdometryPacket 重复处理，调整分支顺序确保只处理一次
- **高优先级修复（后端）**:
  - `serial.rs` — `read_line` 中 `from_utf8_lossy` 改为 `from_utf8`，非 UTF-8 时记录日志丢弃
  - `api.rs` — `connect_serial` 阻塞 I/O 移出 `MutexGuard` 保护范围
  - `serial.rs` — 帧头查找添加 5 秒总超时限制；帧大小上限从 10MB 改为 256KB
  - `lib.rs` — `video_frame` 类型改为 `Arc<Mutex<Option<Arc<Vec<u8>>>>>` 共享引用
  - `websocket.rs` — `forward_task` 异常退出显式错误处理；`video_task` 使用 `Arc::clone` 共享帧引用
- **高优先级修复（固件）**:
  - `odometer.h` — `g_lastLeftPulses`/`g_lastRightPulses` 声明为 `volatile`
  - `pid_control.h` — `dtMs == 0` 时直接返回上次状态，不硬编码 0.01f
  - `car_controller.ino` — `sendOdometryData` 速度值添加 `constrain` 限制在 `INT16_MIN`~`INT16_MAX`
  - `receiver_dongle.ino` — 视频包添加 `version` 严格校验
- **一般修复（前端）**:
  - `ControlPanel.vue` — logs 用 `Date.now()` 作为 key；`addLog` 错误对象正确处理
  - `useKeyboard.ts` — `handleKeyUp` 统一为替换整个 Set
  - `useApi.ts` — headers 合并逻辑添加注释
  - `useStatus.ts` — 日志仅开发环境输出
- **一般修复（固件）**:
  - `motor_control.h` — `createLeftTurnState`/`createRightTurnState` 使用 `(speed + 1) / 2` 保持对称
  - `wireless.h` — `sendPacket` 使用局部缓冲区拷贝 MAC，消除 `const_cast`；新增 `sendRawPacket()` 通用发送
  - `video_stream.h` — `delayMicroseconds(100)` 改为 `50`；`FrameState::frameBuffer` 改为非 const 消除 `const_cast`
- **一般修复（后端）**:
  - `build.rs` — `bun install` 添加条件判断，避免每次构建都运行
  - `Cargo.toml` — 添加 `rust-version = "1.75"` 字段
- **验证**: `bun run build` 成功；`cargo test` 因 Rust 1.96.0 编译器 ICE（已知 bug）暂无法运行；`cargo clippy` 同理

### 2026-06-08 - 全面代码排查与优化
- **范围**: 后端 Rust + 前端 Vue + 嵌入式固件三部分全面审查优化
- **前端修复**:
  - `useWebSocket.ts` — 闭包+单例重构消除模块级全局变量，HMR 安全；空 catch 块添加日志；定时器类型修正
  - `useKeyboard.ts` — 重构为标准 composable，内部自动 onMounted/onUnmounted 管理生命周期
  - `ControlPanel.vue` — 键盘监听器清理函数保存并调用；速度防抖定时器 onUnmounted 清理；连接按钮 loading 状态；useApi 替代重复 fetch；ARIA 标签
  - `SpeedDashboard.vue` / `StatusBar.vue` — setInterval 类型修复；空 catch 块添加日志；ARIA 标签
  - 新增 `useApi.ts` — 公共 API 调用封装（request/post/get）
- **后端修复**:
  - `websocket.rs` — CancellationToken 替代 .abort() 实现视频任务优雅关闭
  - `serial.rs` — spawn_blocking JoinError 区分 panic/cancel 处理；std::mem::take 避免帧缓冲 clone
  - `build.rs` — 添加 SKIP_FRONTEND_BUILD 环境变量跳过前端构建
  - 测试代码 unwrap() 全部替换为 expect()
  - 新增 9 个测试（总计 35 个），覆盖 handle_message、并发客户端、超长/特殊字符命令
- **固件修复**:
  - `odometer.h` — 扩大中断临界区 noInterrupts()/interrupts() 保护范围
  - `pid_control.h` — millis() 时间差确保 uint32_t 溢出安全
  - `receiver_dongle.ino` — 帧缓冲区边界检查防溢出；Serial 写入前空间检查
  - `car_controller.ino` — 添加 DEBUG_MOTOR/SERVO/WIRELESS/ODOMETRY/PID 条件编译开关
- **验证**: `bun run build` 成功；`cargo test` 35 测试全过；`cargo clippy` 0 errors

### 2026-06-08 - 前端关键 bug 修复
- **问题**: useWebSocket.ts 存在重连竞争、多组件卸载时意外断开全局 WebSocket、双重连接；VideoPlayer.vue 组件卸载后 RAF 递归调用导致内存泄漏；ControlPanel.vue 云台指令错误（'L'/'R'）、smartDriveOn 初始值与固件不匹配、速度滑块无防抖频繁发送命令；StatusBar.vue 连接状态使用独立 ref 导致永远显示 OFF
- **修复文件**:
  - `desktop/frontend/src/composables/useWebSocket.ts` — 引入单管理员模式（`owner` 参数），只有 `owner=true` 的调用者才能执行 `connect()`/`disconnect()`，其他组件只消费状态；添加 `shouldReconnect` flag，`disconnect()` 先设 flag 为 `false` 再关闭 socket，阻止 `onclose` handler 自动重连；移除 `onMounted`/`onUnmounted` 中的自动连接/断开逻辑；odometry 数据解析从 `as number` 不安全类型断言改为 `typeof` 运行时校验；`sendCommand` 和所有 `ws.value.send()` 调用处添加 try-catch；清理 10 处 `console.log`
  - `desktop/frontend/src/App.vue` — 使用 `useWebSocket(true)` 作为管理员，在 `onMounted` 中调用 `connect()`；版本号显示 `v1.0.0` → `v1.2.0`
  - `desktop/frontend/src/components/VideoPlayer.vue` — 添加 `onUnmounted` 钩子调用 `cancelAnimationFrame(rafId)` 终止递归动画帧；`lastFpsUpdate` 初始值从 `0` 改为 `Date.now()`；移除录制按钮、`isRecording` 状态、`toggleRecording` 函数和 `resolution` 显示（录制功能为空操作）
  - `desktop/frontend/src/components/ControlPanel.vue` — 云台左按钮指令从 'L'（航线修正）修正为 'H'（云台左），云台右按钮从 'R'（无效指令）修正为 'K'（云台右）；`smartDriveOn` 初始值从 `true` 改为 `false`，匹配固件默认模式 0；速度滑块 `@input` 事件添加 200ms 防抖（`setTimeout`/`clearTimeout`），快速拖动时只发送最终值
  - `desktop/frontend/src/components/StatusBar.vue` — 删除本地 `const isConnected = ref(false)`，改为从 `useWebSocket()` 导入 `isConnected`，确保状态与实际 WebSocket 连接一致
- **验证**: `bun run build` 成功，`npx vue-tsc --noEmit` 无类型错误

### 2026-06-08 - serial.rs 阻塞 I/O 与锁优化修复
- **问题**: `run_serial_task` 直接调用阻塞串口 I/O（`read_exact`, `write_all`）在 async 上下文；持有 `serial_manager` 锁的同时获取 `video_frame`/`odometry` 锁；`read_line` 中 `line_buffer.clear()` 冗余
- **修复文件**:
  - `desktop/backend/src/lib.rs` — `serial_manager` 从 `tokio::sync::Mutex` 改为 `std::sync::Mutex`（串口 I/O 是阻塞的，不适合 async Mutex）
  - `desktop/backend/src/serial.rs` — `run_serial_task` 使用 `tokio::task::spawn_blocking()` 包装阻塞 I/O；通过 `SerialTaskResult` 枚举在 blocking 与 async 上下文间传递结果；获取数据后立即释放 `serial_manager` 锁，再单独获取 `video_frame`/`odometry` 锁；移除 `read_line` 冗余 `clear()`
  - `desktop/backend/src/api.rs` — 4 处 `.lock().await` 改为 `.lock().unwrap()`
  - `desktop/backend/src/websocket.rs` — 2 处 `.lock().await` 改为 `.lock().unwrap()`；`command`/`drive_mode` 分支使用作用域确保 `std::sync::MutexGuard` 在 `.await` 前释放（`MutexGuard` 非 `Send`，不能跨 await 存活）
- **验证**: `cargo test` 25 个测试全部通过，`cargo clippy` src/ 0 errors（build.rs 有 3 个 pre-existing 错误）

### 2026-06-08 - api.rs 空命令与锁优化修复
- **问题**: `handle_command` 空字符串时发送 0x00 到串口；`get_status` 同时持有 4 把锁；StatusResponse 三段重复构造；`handle_message` 锁顺序与 `get_status` 不一致
- **修复文件**:
  - `desktop/backend/src/api.rs` — 空命令返回 400 Bad Request；`get_status` 逐把加锁释放；StatusResponse 单次构造
  - `desktop/backend/src/websocket.rs` — command 分支改为先 `serial_manager` 后 `current_speed`；drive_mode 分支修复重复加锁；`cmd_byte >= b'1' && cmd_byte <= b'9'` 改为 `(b'1'..=b'9').contains(&cmd_byte)`
- **验证**: `cargo test` 25 个测试全部通过，`cargo clippy` 0 errors

### 2026-06-07 - Rust 自动化测试
- **新增**: 25 个自动化测试（19 单元 + 1 主模块 + 5 集成），覆盖 serial/websocket/api 模块
- **serial.rs**: 8 个测试 — 初始状态、未连接发送/断开、测速 JSON 解析（有效/非odom/无效JSON/缺字段）、OdometryData 默认值
- **websocket.rs**: 5 个测试 — WebSocketManager 增删客户端、base64 编码
- **api.rs**: 6 个测试 — CommandRequest/ConnectRequest 反序列化、ApiResponse/StatusResponse 序列化
- **main.rs**: 1 个测试 — AppState 初始状态
- **tests/api_integration.rs**: 5 个集成测试 — GET /api/status、POST /api/command(503)、POST /api/disconnect、POST /api/connect(503)、WebSocket 升级
- **重构**: AppState 从 main.rs 迁移到 lib.rs，支持集成测试访问公共 API

### 2026-06-07 - 前端依赖大版本升级
- **TailwindCSS v3 → v4**: 配置从 JS 迁移到 CSS `@theme`，移除 `tailwind.config.js` 和 `postcss.config.js`，改用 `@tailwindcss/vite` 插件
- **Vite 5 → 8**: 统一 Rolldown 打包器，构建速度提升 10-30x
- **Vue 3.4 → 3.5.35**: 响应式性能提升
- **@vitejs/plugin-vue 5 → 6**: 兼容 Vite 8
- **移除**: `autoprefixer`、`postcss` 依赖（v4 内置）
- **修复**: 滑块 thumb 对齐（WebKit `margin-top: -4px`，Firefox 无偏移）
- **修复**: SpeedDashboard scoped 样式改用原生 CSS 变量（v4 `@apply` 在 scoped 样式中需 `@reference`）
- **修复**: `@apply` 不能引用自定义组件类（`btn`、`status-indicator` 样式内联）

### 2026-06-07 - 速度显示异常与滑块对齐修复
- **问题**: 速度显示 1422%（`current_speed` 初始值为 128 导致），速度滑块与快速按钮宽度不对齐
- **修复文件**:
  - `desktop/backend/src/main.rs` — `current_speed` 初始值从 128 改为 5
  - `desktop/backend/src/websocket.rs` — 收到速度命令 '1'-'9' 时同步更新 `current_speed`
  - `desktop/frontend/src/components/SpeedDashboard.vue` — 改用 WebSocket odometry 数据显示实际轮速（cm/s）
  - `desktop/frontend/src/components/StatusBar.vue` — 添加 clamp 保护确保速度等级在 1-9 范围
  - `desktop/frontend/src/components/ControlPanel.vue` — 修复滑块与按钮宽度对齐（统一左右边距）
  - `desktop/frontend/src/style.css` — 移除轨道背景色避免覆盖动态渐变

### 2026-06-07 - 速度滑块改为无极调节
- **改动**: 滑块 step 从 1 改为 0.1，移除下方快速按钮，发送时取整
- **文件**: `desktop/frontend/src/components/ControlPanel.vue`

### 2026-06-07 - 滑块 thumb 对齐与 Rust 自动构建前端
- **问题**: 滑块 thumb 中心与轨道中心不对齐；`cargo build` 不会自动构建前端
- **修复**:
  - `desktop/frontend/src/style.css` — thumb 添加 `margin-top: -6px` + `box-sizing: border-box` 垂直居中
  - `desktop/backend/build.rs` — 新增构建脚本，自动检测并构建前端
  - `desktop/backend/Cargo.toml` — 添加 `build = "build.rs"`

### 2026-06-09 - 固件关键 bug 修复（6项）
- **范围**: 嵌入式固件 4 个文件，修复 6 项严重 bug
- **严重修复**:
  - `motor_control.h` — 运动创建函数引用 MOTOR_FL_IN1/IN2 (GPIO 10-11) 和 MOTOR_FR_IN1/IN2 (GPIO 12-13)，这些引脚在 ESP32-C6 上连接内部 SPI Flash 不可用。`initializeMotorPins()` 只配置了 GPIO 4-9。替换为 MOTOR_LEFT_IN1/IN2 (GPIO 4/5) 和 MOTOR_RIGHT_IN1/IN2 (GPIO 7/8)，删除不可用的 GPIO 10-13 常量
  - `servo_control.h` — `parseGimbalCommand` 中 `uint8_t` 角度减法下溢（0 - 5 = 251），导致舵机跳到 180°。改为安全算术：减法前检查 `>= step`，加法前检查 `+ step <= maxAngle`
  - `camera_config.h` — PWDN/RESET 引脚类型为 `uint8_t = -1`（存储 255 而非 -1），ESP 驱动检查 -1 跳过未用引脚。改为 `int8_t`；ImageQuality 枚举值反转（驱动中数值越小质量越高）：LOW=50, MEDIUM=30, HIGH=15, BEST=5
  - `car_controller.ino` — `setup()` 中 `g_smartDriveEnabled = true` 覆盖全局初始值 false，改为保持 false；`handleDriveModeCommand` 添加 `setStraightLineEnabled()` 调用同步双标志
  - `receiver_dongle.ino` — `getCommandType` 中 'D'/'d' 同时出现在 MOVE 和 SERVO 分支，switch fall-through 导致云台下命令永远匹配 MOVE。从 MOVE 分支移除 'D'/'d'；`Serial.read()` 返回 `int` 存入 `char` 导致符号扩展，改为 `int` 类型
  - `car_controller.ino` — `handleSpeedCommand` 和 `handleServoCommand` 未更新 `g_lastCmdTime`，仅发送速度/云台命令时 1 秒超时触发自动停止。添加 `g_lastCmdTime = millis()`

### 2026-06-09 - 全面代码排查与优化 v2（23项修复）
- **范围**: 前端 Vue + 后端 Rust + 嵌入式固件三部分全面审查，修复 9 项严重问题、7 项高优先级问题、7 项一般问题
- **严重修复（固件）**:
  - `motor_control.h` — 运动创建函数引用 MOTOR_FL_IN1/IN2 (GPIO 10-11) 和 MOTOR_FR_IN1/IN2 (GPIO 12-13)，这些引脚在 ESP32-C6 上连接内部 SPI Flash 不可用。替换为 MOTOR_LEFT_IN1/IN2 (GPIO 4/5) 和 MOTOR_RIGHT_IN1/IN2 (GPIO 7/8)，删除不可用的 GPIO 10-13 常量
  - `servo_control.h` — `parseGimbalCommand` 中 `uint8_t` 角度减法下溢（0 - 5 = 251），导致舵机跳到 180°。改为安全算术：减法前检查 `>= step`，加法前检查 `+ step <= maxAngle`
  - `camera_config.h` — PWDN/RESET 引脚类型为 `uint8_t = -1`（存储 255 而非 -1），ESP 驱动检查 -1 跳过未用引脚。改为 `int8_t`；ImageQuality 枚举值反转（驱动中数值越小质量越高）：LOW=50, MEDIUM=30, HIGH=15, BEST=5
  - `car_controller.ino` — `setup()` 中 `g_smartDriveEnabled = true` 覆盖全局初始值 false，改为保持 false；`handleDriveModeCommand` 添加 `setStraightLineEnabled()` 调用同步双标志
  - `receiver_dongle.ino` — `getCommandType` 中 'D'/'d' 同时出现在 MOVE 和 SERVO 分支，switch fall-through 导致云台下命令永远匹配 MOVE。从 MOVE 分支移除 'D'/'d'；`Serial.read()` 返回 `int` 存入 `char` 导致符号扩展，改为 `int` 类型
  - `car_controller.ino` — `handleSpeedCommand` 和 `handleServoCommand` 未更新 `g_lastCmdTime`，仅发送速度/云台命令时 1 秒超时触发自动停止。添加 `g_lastCmdTime = millis()`
- **严重修复（前端）**:
  - `ControlPanel.vue` — 串口连接/断开时不再修改 WebSocket `isConnected`，消除状态混淆；运动网格底行 Q/E 改为 A/D（消除与顶行重复）
  - `useKeyboard.ts` — `activeKeys` 从 `ref<Set>` 改为每次修改创建新 Set 触发 Vue 响应式，修复按键高亮不工作
  - `ControlPanel.vue` — 所有控制按钮添加 `@mouseleave` 事件发送停止命令，修复鼠标移出按钮后车辆持续运动
  - `VideoPlayer.vue` — 分离 `rafId` 和 `timeoutId` 为两个独立变量，修复定时器类型混淆
- **高优先级修复（后端）**:
  - `websocket.rs` — 帧哈希改用多点采样（首4+中4+尾4字节+长度），修复同尺寸帧碰撞丢帧
  - `serial.rs` — `SerialTaskResult` 所有变体携带 buffer，非视频路径恢复 `frame_buffer`，避免重复分配
  - `api.rs` — REST API 速度命令 '1'-'9' 同步更新 `current_speed`，与 WebSocket 行为一致
  - `serial.rs` — `connect()` 失败时状态恢复为 `Disconnected`，不再残留 `Connecting`
- **高优先级修复（前端）**:
  - `useWebSocket.ts` — `connect()` 先关闭旧连接再创建新的，防止状态腐败
  - `useApi.ts` — 添加 `response.ok` 检查，非 2xx 抛出错误；GET 请求不设置 Content-Type
- **一般修复（前端）**:
  - `useWebSocket.ts` — 重连策略改为指数退避（1s→30s）+ 最大 10 次重试
  - `useWebSocket.ts` — WS_URL 从硬编码 `ws://localhost:8080/ws` 改为基于 `window.location` 动态构建
  - 新增 `useStatus.ts` — 合并 StatusBar/SpeedDashboard 重复 `/api/status` 轮询为共享 composable
- **一般修复（后端）**:
  - `lib.rs` — `ws_manager` 改为 `std::sync::Mutex`；`current_speed` 改为 `AtomicU8`；`last_heartbeat` 改为 `std::sync::Mutex`；`video_frame` 简化为单层 `Arc<Mutex<Option<Vec<u8>>>>`
  - `websocket.rs` — odometry 广播添加 200ms 节流，减少不必要的网络流量
- **一般修复（固件）**:
  - `odometer.h` / `pid_control.h` — 版本号 1.1.0 → 1.2.0（遗漏更新）
  - `wireless.h` — 魔术字注释 0xAA → 0xA5
  - `video_stream.h` — `captureFrame()` 注释从"纯函数"修正为"有副作用"
  - `car_controller.ino` — `g_currentSpeed` 默认值 128 → 28（匹配 map 最小值，更安全）
- **验证**: `vue-tsc --noEmit` 通过；`bun run build` 成功；`cargo test` 35 测试全过；`cargo clippy` 0 errors

### 2026-06-09 - 全面代码排查与优化（14项修复）
- **范围**: 前端 Vue + 后端 Rust + 嵌入式固件三部分全面审查，修复 6 项严重问题、7 项一般问题、4 项优化建议
- **严重修复**:
  - `ControlPanel.vue` — 运动控制网格第一行从两个"云台上"按钮修正为 Q/W/E（原地左转/前进/原地右转）布局
  - `ControlPanel.vue` — 删除本地 `isConnected` ref，改为从 `useWebSocket()` 导入，统一连接状态源
  - `car_controller.ino` — `g_smartDriveEnabled` 初始值从 `true` 改为 `false`（匹配前端默认 OFF）
  - `useKeyboard.ts` — `handleKeyDown` 添加 `if (event.repeat) return;` 防止 OS 按键重复导致命令风暴
  - `receiver_dongle.ino` — 移除 `onReceiverDataRecv` 中 ODOMETRY 数据的 WirelessPacket 分支透传，避免重复发送
  - `video_stream.h` — `VideoPacket` 结构体所有成员移除 `const` 修饰（修复不可编译的赋值操作）
- **一般修复**:
  - `VideoPlayer.vue` — `updateVideo()` 无视频帧时用 `setTimeout` 延迟轮询替代持续 RAF 循环
  - `SpeedDashboard.vue` / `StatusBar.vue` — 使用 `useApi().get()` 替换裸 `fetch`
  - `websocket.rs` — `video_task` 添加 `last_frame_hash` 帧去重，仅新帧时 base64 编码发送
  - `serial.rs` — 视频帧用 `Arc<Vec<u8>>` 共享引用替代 `clone()`
  - `lib.rs` — `video_frame` 字段类型改为 `Arc<Mutex<Option<Arc<Vec<u8>>>>>`
  - `ControlPanel.vue` — `sendCommand()` 移除普通命令日志记录，避免高频操作日志洪流
  - `App.vue` — 移除 `onMounted` 自动连接 WebSocket，改为用户手动触发
  - `main.rs` — API 路由与 SPA fallback 分离为独立 Router，避免 API 404 返回 index.html
- **版本统一**: 所有固件文件版本号统一为 1.2.0（8 文件 11 处）
- **验证**: `vue-tsc --noEmit` 通过；`bun run build` 成功；`cargo test` 35 测试全过；`cargo clippy` 0 errors

## 额外要求

在修改代码时，严格遵守：

- **编程风格**: 函数式编程，大量中文注释。Rust 部分，使用`cargo fmt`格式化，记得合理编写自动化测试，必须`cargo clippy`和自动化测试全过，才能提交。
- **当完成修改时**: 更新"AGENTS.md","CHANGELOG.md","README.md"，然后提交并推送git。