# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.8.0] - 2026-06-13

### 硬件变更
- **移除舵机**：砍掉 SG90 水平/垂直舵机，简化硬件
- **HardwareSerial 直连**：ESP32-S3 摄像头与 ESP32-C6 车载控制器改为 HardwareSerial (Serial1) 直连（GPIO 14/15），替代 ESP-NOW 无线通信
- **BLE 扫描**：接收器新增 BLE 扫描功能，电脑端可发现周围蓝牙设备

### 固件
- car_controller: 移除舵机代码，新增 HardwareSerial 视频帧接收/转发
- camera_module: 移除 ESP-NOW，改为 Serial1 发送视频帧
- receiver_dongle: 新增 BLE 扫描功能
- wireless.h: 移除 SERVO/CAMERA 相关类型和函数

### 后端
- 新增 BleDevice 数据结构和 BLE 设备管理
- 新增 GET /api/ble-devices 端点
- 新增 ble_scan WebSocket 消息和 ble_devices 广播

### 前端
- 移除云台控制 UI 和 MAC 地址设置 UI
- 新增 BLE 设备扫描按钮和设备列表

### Breaking Changes
- CommandType::SERVO 从无线协议中移除
- DeviceRole::CAMERA 从无线协议中移除
- 云台控制命令（U/J/H/K/C）不再有效
- 摄像头模块不再独立运行 ESP-NOW

## [1.7.4] - 2026-06-13

### Fixed
- **综合代码审计 v9** — 全面审查固件/后端/前端三部分，启用 karpathy-guidelines 深度排查，修复 36 项问题（1 P1 + 4 P2 + 2 Serious + 3 High + 26 P3/Low）
- **P1: 摄像头 SERVO 转发修复** — `camera_module.ino` 移除损坏的 `sendToCar(*packet)` 调用，摄像头仅与接收器配对，CAR_MAC 不在 peer 表中导致 esp_now_send 静默失败
- **P2: 航向锁定角度环绕归一化** — `pid_control.h` headingError 添加 [-PI, PI] 归一化，防止角度跨 0/2PI 边界时车辆旋转大圈
- **P2: MAC 设置局部故障修复** — `wireless.h` setTargetCarMac/setTargetCameraMac 添加 same-MAC 跳过检查和 peer 添加失败时 early return，防止全局 MAC 指向未注册 peer
- **P2: autoCalibrate 除零保护完善** — `odometer.h` 校正系数计算同时检查 leftSpeed/rightSpeed > 0.1f
- **P2: 帧头 JPEG SOI 验证** — `serial.rs` read_frame_data 读取帧数据后验证 JPEG SOI (0xFF 0xD8)，不匹配则触发流对齐恢复，防止串口数据中的 0xAA 0x55 巧合造成帧头误检测
- **Serious: 孤立轮询器逃逸修复** — `useStatus.ts` startPolling 中 await fetchStatus() 后检查 refCount，避免组件在 await 期间卸载导致 interval 定时器孤立运行
- **Serious: 速度发送协议修正** — `ControlPanel.vue` setSpeed 改用 sendSpeed()（type:speed）替代 sendCommand()（type:command），与后端 speed 消息处理对齐
- **High: 串口 WS 失败回滚** — `ControlPanel.vue` connect() 中 wsConnect 失败时自动调用 /api/disconnect 回滚串口连接
- **High: 键盘焦点检查** — `useKeyboard.ts` handleKeyDown 检查 document.activeElement，忽略 INPUT/TEXTAREA 中的按键
- **High: IME 组合检查** — `useKeyboard.ts` handleKeyDown 检查 event.isComposing，防止中文输入法误触发
- **P3: 固件死代码/返回值检查** — `camera_config.h` updateCameraConfig 添加注释；`wireless.h` g_peerInfo 添加 init-only 注释；3 个 .ino 文件 esp_now_register_recv_cb 返回值检查
- **P3: 固件代码质量** — `include` 路径规范化（尖括号改双引号）；`video_stream.h` const_cast 消除（直接写入 packet.data）；`servo_control.h` ServoConfig constexpr 去重
- **P3: 后端优化** — `websocket.rs` 帧哈希改用 SipHash-2-4 DefaultHasher；chrono → std::time::SystemTime；无客户端时跳过视频处理
- **P3: 后端优化** — `Cargo.toml` tower → dev-dependencies；`serial.rs` line_buf 64KB 硬上限 + std::mem::take 替代 clone；`main.rs` dotenvy 错误处理
- **P3: 前端优化** — `package.json` 移除未使用的 pinia；`style.css` 移除死 .status-indicator 类；`SpeedDashboard.vue` 运行时间补充秒数显示；`ControlPanel.vue` 移除 slider 冗余 ARIA 属性
- **文档更新** — 5 个 AGENTS.md 同步：AppState 位置修正（main.rs→lib.rs）、视频缓冲区 4096→32768、composables 列表补全、添加 /api/ports 端点
- 验证: `cargo clippy` 0 warnings；`cargo test` 42 测试全过；`bun run build` 成功

## [1.8.1] - 2026-06-13

### Added
- **前端资源嵌入二进制** — `main.rs` 使用 `rust-embed` 将 `frontend/dist/` 编译进 Rust 可执行文件，`Cargo.toml` 新增 `rust-embed` + `mime_guess` 依赖，移除 `tower-http`（仅用于 `ServeDir`）。exe 可在任意位置运行，无需前端 `dist` 目录伴随
- **SPA fallback 保留** — 所有未匹配路由返回 `index.html`，支持 Vue 客户端路由

### Fixed
- **P1: camera_module.ino Serial1.begin 参数错误** — ESP32 Arduino core 没有 `(baud, config)` 的 2 参数重载，`SERIAL_8N1` 被误作 `rxPin` 导致串口初始化失败。改为 `Serial1.begin(921600)` 使用默认引脚
- **P2: receiver_dongle.ino BLEDevice::init 重复调用** — `performBleScan()` 每次调用都执行 `BLEDevice::init("")`，可能导致资源泄漏。改为在 `setup()` 中初始化一次，扫描时只做扫描
- **P2: car_controller.ino 视频帧接收/转发竞态** — `receiveCameraFrame()` 和 `forwardCameraFrame()` 共享缓冲区，发送期间可能接收新帧覆盖数据。将状态机变量从局部 static 提升为全局 static，并在 `receiveCameraFrame()` 开头添加 `g_cameraFrameReady` 检查，帧就绪时暂停接收

### Fixed
- **P2: DRIVE_MODE 命令原子性修复** — `websocket.rs` drive_mode 从两次独立 `send_command` 改为 `send_bytes(&[b'T', mode_value])` 原子发送，防止中间插入其他命令导致接收器 50ms 超时丢弃
- **P2: ControlPanel WebSocket 连接异常处理** — `ControlPanel.vue` wsConnect() 改为 `await` + try-catch，确保连接失败异常可被捕获而非静默丢弃
- **P3: 固件死代码清理（5项）** — `pid_control.h` 移除死字段 `g_targetHeading`；`motor_control.h` 移除从未调用的 `changeMotorState` 函数；`receiver_dongle.ino` 移除从未置 true 的 `g_isStreaming` 变量；`wireless.h` 移除从未引用的 `TIMEOUT_MS` 常量和 `WirelessState` 中从未更新的 `isConnected`/`lastSeq` 字段
- **P3: 视频流配置注释修正** — `video_stream.h` `VideoStreamConfig::JPEG_QUALITY_MAX/MIN` 注释从"最大/最小JPEG质量"修正为"最大/最小压缩值"，与 ESP32 驱动语义对齐（数值越大 = 压缩越高 = 质量越低）
- **P3: 前端 FPS 去重** — `VideoPlayer.vue` 移除独立 FPS 计算逻辑，统一使用 `useWebSocket().videoFps`，消除重复统计
- **P3: StatusBar 速度回退逻辑修正** — `StatusBar.vue` currentSpeed 的 `|| 5` 回退改用显式 null/undefined 检查，避免合法值 0 被错误替换
- **P3: 后端死配置清理** — `.env` 移除未被任何代码读取的 `VIDEO_FRAME_BUFFER_SIZE` 和 `MAX_VIDEO_PACKET_SIZE`
- **P3: 串口格式符修正** — `receiver_dongle.ino` odometry JSON `%u` 格式符对 `uint16_t` 添加 `static_cast<unsigned int>()` 显式转换，消除隐式提升歧义
- 验证: `cargo clippy` 0 warnings；`cargo test` 42 测试全过；`bun run build` 成功

### Fixed
- **P0: 视频包校验和传输修复** — `video_stream.h` 校验和写入位置从 `packet.checksum`（偏移138）改为实际发送末字节，`sendSize` 增加校验和 1 字节；`receiver_dongle.ino` 读取改为 `data[len-1]`，修复非满载包校验和从未传输 & 越界读取 UB，视频帧最后一个分包不再丢失
- **P1: videoFps 死状态修复** — `useWebSocket.ts` videoFps 添加实际每秒帧率统计更新逻辑，StatusBar FPS 指示器恢复正常
- **P1: 主循环延迟优化** — `car_controller.ino` loop() 中 delay(10) → delay(1)，命令响应延迟降低
- **P2: speed 死代码消除** — `websocket.rs` speed 消息类型现在也通过串口发送速度等级字符，sendSpeed() API 已可用
- **P2: 注释修正** — `car_controller.ino` + `receiver_dongle.ino` setup() 云台按键注释从 U/D/L/R/C 修正为 U/J/H/K/C
- **P2: 静态初始值修正** — `pid_control.h` g_driveMode 静态初始值从 STRAIGHT_LINE 改为 NORMAL，与运行时一致
- **P3: 非owner调用警告** — `useWebSocket.ts` 非管理员组件调用 connect/disconnect 时 DEV 模式输出 console.warn
- **P3: 代码整洁** — `car_controller.ino` 清理 loop() 中残留空白行
- 验证: `cargo clippy` 0 warnings；`bun run build` 成功

### Fixed
- **v6 综合代码审计修复（46项）** — 修复 1 项 P0、6 项 P1、12 项 P2、27 项 P3
- **P0**: `receiver_dongle.ino` DRIVE_MODE 命令包改用 `createCommandPacket()` 构造，修复 magic/version/checksum 缺失
- **P1**: MAC 动态配置 peer 先删后加、测速校准系数去重、摄像头日志防重入、串口断开竞态修复、串口按钮状态独立
- **P2**: extern 解耦、死字段移除、空 switch 删除、除零保护、命令错误消息优化、行缓冲数据保留、MAC 原子发送、panic 日志、build.rs 路径修正、retryCount 重置、GIMBAL_KEYS Set、port_list 类型守卫
- **P3**: 固件 6 项 + 后端 13 项 + 前端 8 项（详见 checklist.md）
- 验证: `cargo check` 通过；`cargo test` 42 测试全过；`bun run build` 成功

### Fixed
- **P1: 视频包校验和验证** — `receiver_dongle.ino` 接收端添加校验和验证，使用 `len - 1` 计算（排除 checksum 字段），与发送端对齐，损坏包静默丢弃防止花屏
- **P3: 箭头键临时数组优化** — `useKeyboard.ts` 定义 `PREVENT_DEFAULT_KEYS` 常量 Set，避免每次 keydown 创建新数组
- **P2: 帧捕获错误恢复** — `video_stream.h` 添加连续失败计数，超过 10 次自动重启摄像头硬件，修复摄像头故障后无法恢复
- **P2: 非标准包日志** — `car_controller.ino` onDataRecv 添加非标准长度包日志（DEBUG_WIRELESS 开关控制），便于调试
- **P3: 串口重启指数退避** — `main.rs` 串口任务重启从固定 3 秒改为指数退避（3s→60s 最大），防止持续失败时频繁重试
- **clippy 警告** — `websocket.rs` test_base64_encode 移除 needless borrow

### Fixed
- **dead_code 清理** — `websocket.rs` 移除未使用的 `base64_encode` 函数；测试模块内联 Base64 编码并添加 `use base64::Engine;` 导入，`cargo clippy` 0 warnings
- **P0: HEADING_LOCK PID参数修正** — `pid_control.h` HEADING_LOCK 模式使用正确的 HEADING_PID 参数和 g_headingPidState 状态变量，修复航向锁定功能名存实亡
- **P0: 视频校验和范围修复** — `video_stream.h` 校验和计算改为仅覆盖实际发送字节（sendSize - 1），修复发送端与接收端校验和不匹配
- **P0: 串口锁重构(take/return)** — `serial.rs` read_next 改为独立关联函数，run_serial_task 使用 take/return 模式避免长时间持 serial_manager 锁，修复视频接收期间 API 请求无响应
- **P0: 帧读取流对齐恢复** — `serial.rs` 新增 resync_stream 流对齐恢复，帧读取失败后跳过残留字节直到找到下一个帧头，修复一次失败导致后续所有帧错位
- **P1: DRIVE_MODE超时保护** — `receiver_dongle.ino` DRIVE_MODE 添加 50ms 超时等待读取模式值，修复命令静默丢弃
- **P1: 视频包校验和验证** — `receiver_dongle.ino` 视频包新增校验和验证，损坏包静默丢弃
- **P1: 航向锁定UI三态按钮** — `ControlPanel.vue` 行走模式从布尔开关改为三态按钮（普通/直线/锁定），航向锁定模式 UI 可访问
- **P1: 重连定时器清理** — `useWebSocket.ts` onopen 中清理 reconnectTimer，防止手动重连后定时器触发创建多余连接
- **P1: 心跳去冗余测速** — `car_controller.ino` STATUS 心跳不再触发 sendOdometryData()，消除冗余测速上报
- **P1: odometry Mutex优化** — `lib.rs` + `serial.rs` + `api.rs` + `websocket.rs` odometry 从 tokio::sync::Mutex 改为 std::sync::Mutex（不跨 .await 持锁）
- **P2: 帧头扫描简化** — `serial.rs` 帧头扫描嵌套逻辑已随 read_next 重构自然简化
- **P2: static变量风格** — `video_stream.h` static 全局变量风格与 wireless.h 保持一致（当前仅单翻译单元包含，风险可控）
- **P2: 串口重启退避** — `main.rs` 串口任务无限重启已有 3 秒退避，当前设计下暂不添加指数退避
- **P0: ESP-NOW 网络拓扑修复** — `wireless.h` Receiver 角色初始化时同时添加 Car 和 Camera 两个 Peer，修复云台控制转发静默失败
- **P0: DRIVE_MODE 协议重构** — 行走模式切换命令从 'M'/'L'/'B' 改为专属字节 'T'，消除与 MAC_CONFIG 的 'M' 冲突。协议：先发 'T' 标识，再发模式值（0/1/2）；`receiver_dongle.ino` 实现 DRIVE_MODE 转发逻辑
- **P0: 串口数据流解析器重构** — `serial.rs` 引入 BufReader + 统一缓冲状态机，修复帧头重叠遗漏（0xAA 0xAA 0x55）和视频/测速数据互斥吞没问题
- **P0: 串口连接自动触发 WebSocket** — `ControlPanel.vue` 串口连接成功后自动触发 WebSocket 连接，补齐实时数据推送入口
- **P0: 视频缓冲区扩大** — `receiver_dongle.ino` 视频缓冲区从 4KB 扩大到 32KB，匹配后端帧大小上限
- **P0: 紧急停止显式解除** — `car_controller.ino` 紧急停止改为仅运动命令显式解除，移除 500ms 自动恢复安全隐患
- **P1: 航向角归一化** — `odometer.h` 航向角 fmod 归一化到 [0, 2π)，防止 int16_t 溢出
- **P1: MAC 配对表更新** — `wireless.h` setTargetCarMac/setTargetCameraMac 添加 esp_now_mod_peer 调用更新配对表
- **P1: HEADING_LOCK 航向锁定实现** — `pid_control.h` 实现 HEADING_LOCK 模式（航向 PID 控制）
- **P1: SERVO 分支命令统一** — `receiver_dongle.ino` SERVO 分支移除 'L'/'R' 历史兼容命令，统一为 H/K/U/J/C
- **P1: CameraConfiguration const 移除** — `camera_config.h` CameraConfiguration 移除 const 成员修饰，允许运行时切换配置
- **P1: dotenv 时序修复** — `main.rs` dotenvy::dotenv() 移至 tracing_subscriber 之前，修复 RUST_LOG 配置失效
- **P1: 命令发送失败感知** — `websocket.rs` handle_message 命令发送失败返回错误响应，前端可感知
- **P1: 串口任务自动重启** — `main.rs` 串口任务退出后自动重启（3秒延迟），防止"假死"
- **P1: MAC 配置帧边界** — `websocket.rs` + `receiver_dongle.ino` MAC 配置增加帧边界标识（0xFF + 长度字节），防止数据注入
- **P1: build.rs 锁文件检测** — `build.rs` 锁文件检测优先级改为 bun.lockb → bun.lock → .package-lock.json
- **P1: 速度初始值同步** — `useStatus.ts` 速度初始值从后端 /api/status 同步，消除硬编码
- **P1: setSpeed 连接检查** — `ControlPanel.vue` setSpeed 添加 isConnected 检查
- **P1: headers 深度合并** — `useApi.ts` headers 深度合并，保留默认 Content-Type
- **P1: 运行时长基于后端** — `SpeedDashboard.vue` 运行时长改为基于后端 uptime 字段
- **P1: heartbeatTimer 清理** — `useWebSocket.ts` connect() 关闭旧连接前先清理 heartbeatTimer
- **P2: Base64 共享编码** — `websocket.rs` + `serial.rs` + `lib.rs` Base64 编码移至串口任务，广播 Arc<String> 避免每客户端重复编码
- **P2: /api/ports 缓存** — `api.rs` /api/ports 使用 AppState 缓存
- **P2: 增量平均速度** — `SpeedDashboard.vue` 平均速度改用 runningSum 增量计算
- **P2: 校准系数约束** — `odometer.h` autoCalibrate 修正系数约束在 0.5~2.0
- **P2: 移除 Pinia 依赖** — `main.ts` 移除未使用的 Pinia 依赖
- **P2: 移除死代码** — `motor_control.h` 移除未使用的 parseCommandWithSpeed 函数
- **P2: 心跳不触发额外测速** — `receiver_dongle.ino` 心跳不触发额外 sendOdometryData（已确认无需修改）
- **P2: BufReader 减少系统调用** — `serial.rs` BufReader 包装串口端口减少系统调用
- **P3: appearance 修复** — `style.css` 移除与 accent-color 矛盾的 appearance: none
- **P3: 箭头键修复** — `useKeyboard.ts` 箭头键 preventDefault 大小写修复
- **P3: drive_mode 回退** — `websocket.rs` drive_mode 未知模式回退到普通模式
- **P3: duty 溢出修复** — `servo_control.h` duty 计算使用 uint64_t 中间值防溢出
- **P3: volatile 清理** — `odometer.h` g_lastLeftPulses/g_lastRightPulses 移除 volatile 误标

### Added
- **后端自动串口扫描** — `serial.rs` 新增 `run_port_scan_task` 后台任务，每秒扫描可用串口，列表变化时更新状态
- **WebSocket 串口列表广播** — `websocket.rs` `video_task` 中检测串口列表变化，自动广播 `{"type":"port_list","ports":[...]}` 给所有前端客户端
- **前端被动接收串口列表** — `useWebSocket.ts` 新增 `availablePorts` 状态，处理 `port_list` 消息；`ControlPanel.vue` 下拉框优先使用 WebSocket 推送的列表
- **MAC 地址动态配置** — 前端新增 MAC 地址输入框（格式 `AA:BB:CC:DD:EE:FF`）和"设置MAC"按钮，支持 `localStorage` 持久化
- **WebSocket mac_config 协议** — `useWebSocket.ts` 新增 `sendMacConfig`，后端 `websocket.rs` 解析并通过串口转发（'M' + 6字节MAC）
- **固件 MAC 地址运行时配置** — `wireless.h` MAC 地址从 `constexpr` 改为可变数组，新增 `setTargetCarMac`/`setTargetCameraMac`；`receiver_dongle.ino` 新增 'M' 命令读取6字节MAC并更新目标地址
- **MAC 地址解析辅助函数** — `websocket.rs` 新增 `parse_mac_address`，支持冒号分隔和无分隔符两种格式
- **后端测试** — 新增 7 个测试（`parse_mac_address` 5个 + `mac_config` 2个 + `AppState` 初始串口列表1个），总计 43 个测试全部通过

### Added
- **ControlPanel.vue 串口扫描** — 添加"扫描"按钮调用 `GET /api/ports`，页面加载时自动扫描一次，解决此前只能手动输入串口名称的问题

### Fixed
- **api.rs port_name 所有权错误** — `connect_serial` 中 `port_name` 被 move 进 `spawn_blocking` 闭包后闭包外仍被引用（E0382），闭包前添加 `port_name.clone()` 解决
- **receiver_dongle.ino 'D' 命令分类错误** — 'D' 从 SERVO 移到 MOVE 分支（'D' 是右转，不是云台下）
- **receiver_dongle.ino H/J/K 命令未识别** — `parseSerialCommand` 和 `getCommandType` 添加 H/J/K 云台命令
- **servo_control.h 缺少 'J' 云台下处理** — 添加 `case 'J': case 'j':` 与 'D' 相同逻辑
- **pid_control.h 初始状态不一致** — `g_straightLineEnabled` 改为 `false`，`g_driveMode` 改为 `NORMAL`，与 car_controller 同步
- **video_stream.h ESP-NOW 广播误发** — 改为指定接收器 MAC 地址，避免车载端误解析视频包
- **receiver_dongle.ino VideoFrameBuffer 内存泄漏** — `new[]` 分配改为静态数组
- **api.rs connect_serial 阻塞 I/O** — `serialport::open()` 移入 `spawn_blocking`，避免阻塞 async 运行时
- **websocket.rs drive_mode 协议不对齐** — 添加注释说明双字节发送逻辑（模式字符 + 模式值）
- **car_controller.ino updateOdometer 过频** — 移到定时条件内，与测速上报同频
- **odometer.h getCurrentOdometry 非原子读取** — 扩大 `noInterrupts` 保护范围到所有共享变量
- **useStatus.ts StatusData 接口不匹配** — 扩展为与后端 StatusResponse 完全对齐的字段
- **SpeedDashboard.vue shift() 性能差** — 改为 `slice(-MAX_SAMPLES)` 截断
- **camera_module.ino handleCameraCommand 空实现** — 添加云台命令转发和状态查询逻辑
- **motor_control.h parseCommandWithSpeed 副作用** — 移除 `Serial.printf` 调试输出
- **video_stream.h adjustQuality 死代码** — 在 `updateStreaming` 中调用实现动态质量调整
- **receiver_dongle.ino SerialCommand const 成员** — 移除 `const` 修饰符，允许赋值操作
- **wireless.h static 全局变量** — 改为 `inline` 变量，确保多翻译单元单一定义
- **StatusBar.vue fps 引用已删除字段** — 改为从 `useWebSocket().videoFps` 获取
- **useWebSocket.ts 缺少 videoFps** — 添加 `videoFps` ref，VideoPlayer 同步更新

### Added
- **api.rs list_ports 端点** — `GET /api/ports` 列出可用串口，使用 `spawn_blocking` 避免阻塞
- **main.rs /api/ports 路由** — 注册新端口列表 API
- **useWebSocket.ts videoFps** — 全局视频帧率状态，供 StatusBar 等组件消费

### Changed
- **字体升级** — Inter → Space Grotesk（显示），Fira Code → JetBrains Mono（等宽），工业科技风
- **控制按钮微交互** — 激活态添加 cyan glow 阴影效果
- **视频区域扫描线** — 添加半透明扫描线纹理，增强科技感
- **ControlPanel.vue onUnmounted 未 await** — `disconnect()` 改为 `.catch(() => {})` 处理 Promise rejection
- **ControlPanel.vue handleSpeedInput 重复** — 直接读取 `currentSpeed.value`，移除 `event.target`/`parseFloat`
- **VideoPlayer.vue RAF 空转** — 移除 `requestAnimationFrame` 循环，改用 `watch(videoFrame)` 监听帧变化
- **useWebSocket.ts 旧连接清理** — 关闭旧连接后设 `shouldReconnect = false` 防误触发重连
- **useWebSocket.ts 命令静默丢弃** — `sendCommand`/`sendSpeed`/`sendDriveMode` 返回 `boolean`
- **servo_control.h 云台命令不匹配** — 'L'/'R' 改为 'H'/'K'，与前端一致
- **car_controller.ino OdometryPacket 强转** — 移除 `reinterpret_cast`，改用 `sendRawPacket()` 通用发送
- **video_stream.h VideoPacket 冗余数据** — 只发送实际有效大小，不发送整个 128 字节
- **receiver_dongle.ino dataLen 未边界检查** — 添加 `packet->dataLen <= MAX_PACKET_SIZE` 验证
- **receiver_dongle.ino OdometryPacket 重复处理** — 调整分支顺序，确保只处理一次
- **serial.rs from_utf8_lossy 数据丢失** — 改为 `String::from_utf8`，非 UTF-8 时记录日志丢弃
- **api.rs connect_serial 锁持有过长** — 阻塞 I/O 移出 `MutexGuard` 保护范围
- **serial.rs 帧头查找无超时** — 添加 5 秒总超时限制；帧大小上限从 10MB 改为 256KB
- **lib.rs video_frame 未用 Arc 共享** — 类型改为 `Arc<Mutex<Option<Arc<Vec<u8>>>>>`
- **websocket.rs forward_task 错误忽略** — 显式错误处理，记录日志
- **websocket.rs video_task clone 整帧** — 使用 `Arc::clone` 共享引用
- **odometer.h g_lastLeftPulses 非 volatile** — 声明为 `volatile`
- **pid_control.h dt==0 硬编码** — 直接返回上次状态，不硬编码 0.01f
- **car_controller.ino sendOdometryData 溢出** — 速度值添加 `constrain` 限制
- **receiver_dongle.ino 视频包误判** — 添加 `version` 严格校验
- **ControlPanel.vue logs key** — 用 `Date.now()` 作为 key 替代 index
- **ControlPanel.vue addLog 错误对象** — `e instanceof Error ? e.message : String(e)`
- **useKeyboard.ts activeKeys 不统一** — `handleKeyUp` 统一替换整个 Set
- **useStatus.ts 日志暴露** — 仅开发环境输出
- **motor_control.h speed/2 精度** — 改为 `(speed + 1) / 2` 保持对称
- **wireless.h const_cast** — 使用局部缓冲区拷贝 MAC；新增 `sendRawPacket()`
- **video_stream.h const_cast** — `FrameState::frameBuffer` 改为非 const
- **video_stream.h 延迟** — `delayMicroseconds(100)` 改为 `50`
- **build.rs bun install** — 添加条件判断，避免每次构建都运行
- **Cargo.toml** — 添加 `rust-version = "1.75"`
- **car_controller.ino g_currentSpeed 默认过高** — 默认值从 128 改为 28（匹配 map 最小值）

### Changed
- **useWebSocket.ts 重连策略** — 固定 5 秒重试改为指数退避（1s→30s）+ 最大 10 次重试
- **useWebSocket.ts WS_URL** — 从硬编码 `ws://localhost:8080/ws` 改为基于 `window.location` 动态构建
- **lib.rs Mutex 类型统一** — `ws_manager` 改为 `std::sync::Mutex`；`current_speed` 改为 `AtomicU8`；`last_heartbeat` 改为 `std::sync::Mutex`；`video_frame` 简化为单层 Arc
- **websocket.rs odometry 广播节流** — 添加 200ms 间隔限制，减少不必要的网络流量

### Added
- **useStatus.ts composable** — 合并 StatusBar/SpeedDashboard 重复 `/api/status` 轮询为共享数据源

### Fixed (firmware misc)
- **odometer.h / pid_control.h** — 版本号 1.1.0 → 1.2.0（遗漏更新）
- **wireless.h** — 魔术字注释 0xAA → 0xA5
- **video_stream.h** — `captureFrame()` 注释从"纯函数"修正为"有副作用"

### Added
- **测速模块**（`odometer.h`）- 完整的编码器测速系统
  - 霍尔/红外编码器中断读取（GPIO 0/1）
  - 左右轮实时速度计算（RPM + mm/s）
  - 行走距离累计
  - 航向角计算（基于差速推算）
  - 自动校准功能（补偿左右轮速度差异）

- **PID 控制器**（`pid_control.h`）- 直线行走修正系统
  - 位置式PID算法，防积分饱和
  - 直线修正模式：自动补偿左右轮速度差
  - 航向锁定模式：保持固定航向
  - 智能差速输出：根据PID修正值调整左右电机PWM

- **智能行走系统** - 不同电机速度差异的自动补偿
  - 直线修正：前进/后退时自动保持直线
  - 差速运动函数：`createDifferentialState()` 支持左右轮独立PWM
  - 前端开关：可随时启用/禁用智能修正

- **测速数据传输链路**
  - 固件：OdometryPacket 结构体，ESP-NOW 发送测速数据
  - 接收器：JSON格式转发测速数据到PC（`{"t":"odom","ls":...,"rs":...,"hd":...,"dist":...}`)
  - 后端：serial.rs 解析测速JSON行，AppState存储测速数据
  - WebSocket：广播 odometry 类型消息到前端
  - API：/api/status 返回左右轮速度、航向、距离、命令数

- **4个测速模块前端显示**（SpeedDashboard.vue）
  - 当前速度：实时显示左右轮速度 + 进度条
  - 最高速度：记录最高速度 + 重置按钮
  - 平均速度：历史平均 + 航向角显示
  - 运行信息：运行时长 + 行走距离 + 命令数

- **全屏自适应UI改造**
  - 100vh 全视口布局，无滚动
  - 右侧面板适配 SpeedDashboard 模块
  - 紧凑控制面板（control-key-sm 样式）

## [1.3.0] - 2026-06-08

### Added
- **新增 `useApi.ts` composable** — 公共 API 调用封装（request/post/get），替代组件内重复 fetch 调用
- **新增 9 个后端测试** — 总计 35 个测试，覆盖 handle_message、并发客户端、超长/特殊字符命令
- **ARIA 无障碍标签** — ControlPanel、VideoPlayer、SpeedDashboard、StatusBar 添加 role/aria-label/aria-live 属性
- **固件调试开关** — car_controller.ino 添加 DEBUG_MOTOR/SERVO/WIRELESS/ODOMETRY/PID 条件编译宏
- **SKIP_FRONTEND_BUILD 环境变量** — build.rs 支持跳过前端构建（加速 CI/纯后端开发）

### Changed
- **useWebSocket.ts 重构** — 闭包+单例模式消除模块级全局变量，HMR 安全
- **useKeyboard.ts 重构** — 标准 composable，内部自动 onMounted/onUnmounted 管理生命周期
- **websocket.rs 视频任务** — CancellationToken 替代 .abort() 实现优雅关闭
- **serial.rs 帧缓冲** — std::mem::take 替代 clone，减少内存分配
- **测试代码质量** — 所有 unwrap() 替换为 expect() 提供错误上下文

### Fixed
- **ControlPanel.vue** — 键盘监听器 onUnmounted 未清理；速度防抖定时器泄漏；连接按钮无 loading 反馈
- **SpeedDashboard.vue / StatusBar.vue** — setInterval 类型不安全；空 catch 块吞掉错误
- **useWebSocket.ts** — 空 catch 块无日志；定时器类型不匹配
- **serial.rs** — spawn_blocking JoinError 未区分 panic 和 cancel 场景
- **odometer.h** — 中断临界区范围不足，可能读到不一致的编码器数据
- **pid_control.h** — millis() 时间差计算在 uint32_t 溢出时得到错误结果
- **receiver_dongle.ino** — 帧缓冲区无边界检查可能溢出；Serial 写入无空间检查可能阻塞

## [1.2.2] - 2026-06-08

### Fixed
- **serial.rs 阻塞 I/O 修复** — `run_serial_task` 使用 `tokio::task::spawn_blocking()` 包装阻塞串口 I/O，避免阻塞 Tokio 运行时
- **serial.rs 锁优化** — `serial_manager` 改用 `std::sync::Mutex`，读取数据后立即释放锁，再单独获取 `video_frame`/`odometry` 锁，消除同时持有多把锁的情况
- **移除 read_line 冗余 clear()** — 删除 `line_buffer.clear()` 重复调用
- **build.rs 改进** — 添加 `rerun-if-changed` 监控 `index.html` 和 `tsconfig.json`，构建失败时返回非零退出码

## [1.2.1] - 2026-06-08

### Fixed
- **api.rs 空命令处理** — `handle_command` 空字符串时不再发送 0x00 到串口，改为返回 400 Bad Request
- **api.rs StatusResponse DRY** — 三段重复构造抽取为单次构造，通过 match 统一设置变化的 serial_status/port_name/baud_rate
- **api.rs 锁争用优化** — `get_status` 从同时持有 4 把锁改为逐把加锁、复制数据后立即释放
- **websocket.rs 锁顺序一致** — `handle_message` command 分支改为先 `serial_manager` 后 `current_speed`，与 `get_status` 顺序一致
- **websocket.rs drive_mode 死锁** — 修复同一 Mutex 重复加锁（`manager2` 改为复用 `manager`）
- **websocket.rs 错误指令字符** — drive_mode 2 从 'H'（云台左）修正为 'B'（航向锁定模式）
- **useWebSocket 生命周期重构** — 引入单管理员模式（`owner` 参数），只有 `owner=true` 才能执行 `connect()`/`disconnect()`，防止多组件卸载时意外断开全局连接
- **useWebSocket 重连竞争** — 添加 `shouldReconnect` flag，`disconnect()` 先设 flag 为 `false` 再关闭 socket，阻止 `onclose` 自动重连
- **VideoPlayer RAF 泄漏** — 添加 `onUnmounted` 钩子调用 `cancelAnimationFrame`，修复组件卸载后递归动画帧持续运行导致的内存泄漏
- **VideoPlayer FPS 初始化** — `lastFpsUpdate` 从 `0` 改为 `Date.now()`，避免首次 FPS 计算异常
- **ControlPanel 云台指令** — 云台左按钮从 'L'（航线修正）修正为 'H'，云台右按钮从 'R'（无效指令）修正为 'K'
- **ControlPanel smartDriveOn** — 初始值从 `true` 改为 `false`，匹配固件默认无修正模式
- **ControlPanel 速度滑块防抖** — 添加 200ms 防抖，快速拖动时只发送最终值，减少串口命令流量
- **StatusBar 连接状态** — `isConnected` 从本地 `ref(false)` 改为从 `useWebSocket()` 导入，确保状态与实际 WebSocket 一致
- **useWebSocket 类型安全** — odometry 解析从 `as number` 不安全断言改为运行时 `typeof` 校验
- **useWebSocket 错误处理** — `sendCommand` 和所有 `ws.value.send()` 调用处添加 try-catch，防止连接异常时抛出未捕获错误
- **移除录制空操作** — 移除 VideoPlayer 录制按钮和 `isRecording` 状态（功能仅为翻转 boolean，无实际录制逻辑）
- **clippy 警告** — 为 `AppState`、`SerialManager`、`WebSocketManager` 添加 `Default` trait 实现；范围检查改为 `(b'1'..=b'9').contains(&cmd_byte)`

### Changed
- **版本号统一至 1.2.0** — `Cargo.toml`、`package.json`、`main.rs`、`App.vue` 同步；`api.rs` 硬编码版本改为 `env!("CARGO_PKG_VERSION")` 实现单一来源

## [1.2.0] - 2026-06-07

### Changed
- **前端依赖大版本升级** — TailwindCSS v3 → v4（CSS-first 配置 + Oxide 引擎），Vite 5 → 8（Rolldown 统一打包器），Vue 3.4 → 3.5.35
- 前端构建产物直接输出到后端目录
- 后端静态文件服务支持 SPA fallback
- Web UI 自适应布局，不同屏幕无需滚动
- 速度控制显示改为百分比（基于 1-9 级别映射到 0-100%）
- 云台下按钮命令从 'D' 修正为 'J'

### Fixed
- 滑块 thumb 对齐 — WebKit `margin-top` 从 `-6px` 改为 `-4px`，Firefox 移除 `margin-top`
- TailwindCSS v4 兼容 — `@apply` 不能引用自定义组件类，改为内联样式；SpeedDashboard scoped 样式改用原生 CSS 变量
- 移除废弃依赖 — `autoprefixer`、`postcss`（TailwindCSS v4 内置），`tailwind.config.js`、`postcss.config.js`（迁移到 CSS `@theme`）
- 速度显示异常 — `current_speed` 初始值 128 导致显示 1422%，改为 5（速度等级）
- 速度命令同步 — WebSocket 收到 '1'-'9' 命令时同步更新后端 `current_speed`
- SpeedDashboard 数据 — 改用 WebSocket odometry 数据显示实际轮速（cm/s），移除 `/api/status` 轮询
- StatusBar 速度显示 — 添加 clamp 保护（1-9），防止异常值显示
- 速度滑块对齐 — 滑块轨道与快速按钮统一左右边距（`ml-5 mr-5`），确保视觉对齐
- 速度滑块无极调节 — step 从 1 改为 0.1，移除下方快速按钮，发送固件时取整
- 滑块 thumb 对齐 — 添加 `margin-top: -6px` + `box-sizing: border-box`，thumb 中心与轨道中心对齐
- Rust 自动构建前端 — 新增 `build.rs`，`cargo build` 时自动检测并构建前端（支持 bun）
- 修复前端未使用变量导致的 `vue-tsc` 编译错误
- 修复 axum 0.8 中 `nest_service` 在根路径不再支持的问题

## [1.1.0] - 2026-06-07

### Changed
- 升级 Rust 后端依赖到 axum 0.8 / tower 0.5 / tower-http 0.6
- 重构 WebSocket 处理为 mpsc channel 模式（适配 axum 0.8 的 SplitSink 变化）
- Message 类型适配 axum 0.8（Utf8Bytes/Bytes）
- 从 tower-http features 中移除未使用的 cors 和 trace

### Fixed
- 修复所有编译警告（未使用导入、不可达模式、未使用变量）
- 修复 uptime 硬编码为 0 的问题（使用 started_at 计算实际运行时间）
- 添加 base64::Engine trait 导入以适配 base64 0.22

### Removed
- 移除 broadcast_video_frames 死代码函数及对应 spawn 调用

## [1.0.0] - 2026-06-07

### Added
- **智能车嵌入式控制系统** - 基于 ESP32-C6 的完整车载控制固件
  - 函数式编程风格电机控制（`motor_control.h`）
  - L298N 双电机驱动支持（4个直流电机控制）
  - 差速转向实现（左转/右转/原地旋转）
  - WASD 命令解析系统
  - 速度分级控制（1-9级）
  - 自动超时保护（1秒无命令自动停止）
  - 紧急停止功能
  
- **ESP-NOW 无线通信协议**（`wireless.h`）
  - 自定义二进制通信协议
  - 数据包格式：魔术字 + 版本 + 类型 + 数据 + 速度 + 序列号 + 校验和（12字节）
  - 支持命令类型：MOVE / SPEED / STOP / STATUS / ODOMETRY / DRIVE_MODE / MAC_CONFIG
  - 设备角色定义：CAR / RECEIVER
  - 自动重连机制

- **ESP32-S3 CAM 视频传输系统**（`camera_module.ino`）
  - OV2640 摄像头初始化与配置
  - 多分辨率支持（QQVGA 到 UXGA）
  - 动态 JPEG 质量调整
  - 通过 Serial1 直接发送完整视频帧到车载控制器
  - 帧率控制（目标 30 FPS）
  - 帧统计与丢帧检测
  
- **电脑端接收器固件**（`receiver_dongle.ino`）
  - ESP32-C6 USB 串口桥接
  - 高速串口通信（921600 波特率）
  - 视频帧转发（帧头 0xAA 0x55 + 帧大小 + 帧数据）
  - 命令路由（串口命令 → ESP-NOW 转发）
  - 心跳包机制
  
- **Rust 桌面后端**（`desktop/backend/`）
  - Axum Web 框架 HTTP 服务器
  - WebSocket 实时通信（视频传输 + 命令控制）
  - 串口通信管理（`serialport` 库）
  - 视频帧 Base64 编码
  - 心跳保活机制
  - RESTful API（命令发送 / 状态查询 / 串口连接）
  - 多客户端 WebSocket 管理
  - CORS 跨域支持
  - 静态文件服务（前端构建产物）
  
- **Vue 前端 Web UI**（`desktop/frontend/`）
  - Vue 3 + Vite + TypeScript 技术栈
  - TailwindCSS 深色主题界面
  - 实时视频播放器（WebSocket 接收 Base64 JPEG 帧）
  - 完整控制面板
    - WASD 键盘控制（支持物理按键映射）
    - 鼠标点击控制（按钮式）
    - 速度滑块控制（1-9级）
    - 三态行走模式（普通/直线/锁定）
    - BLE 扫描按钮
    - 紧急停止按钮
  - 状态栏显示
    - WebSocket 连接状态
    - 串口连接状态
    - 实时帧率（FPS）
    - 当前速度
    - 接收帧数
  - 系统日志面板
  - 截图功能（下载当前视频帧）
  - 录制功能（界面预留）
  
- **项目文档**
  - `README.md` - 完整项目说明（架构、安装、使用）
  - `docs/hardware.md` - 详细硬件接线图
  - `.gitignore` - Git 忽略配置
  - `.env` - 后端环境配置

### Technical Details

#### 嵌入式固件特点
- **函数式编程风格**：大量 const 数据、纯函数、显式状态传递
- **中文注释**：所有函数、结构体、枚举均有详细中文注释
- **模块化设计**：电机、舵机、无线通信完全分离
- **类型安全**：使用 enum class 替代 enum，避免隐式转换
- **防错误机制**：电机超时保护、舵机角度限制、数据包校验

#### 通信协议
- 控制命令：单字节字符（W/A/S/D/Q/E/空格/1-9/U/D/L/R/C）
- 数据包结构：8字节固定长度 + 校验和
- 视频帧格式：帧头(2) + 大小(4) + 数据(N)
- 传输方式：ESP-NOW 无线 + USB 串口

#### 前端架构
- **组合式函数**：useWebSocket（WebSocket 连接管理）、useKeyboard（键盘事件）、useApi（API 封装）、useStatus（状态轮询）
- **响应式设计**：TailwindCSS 工具类，支持暗色主题
- **性能优化**：watch 监听视频帧变化

### Hardware Requirements
- ESP32-C6 开发板 × 2（车载 + 接收器）
- ESP32-S3 CAM（摄像头模块）
- L298N 电机驱动模块 × 2
- 直流减速电机 × 4
- 7.4V 锂电池
- 5V 稳压模块

### Dependencies

#### 嵌入式（Arduino IDE）
- ESP32 Board Package（支持 C6 和 S3）
- ESP32Camera 库
- ESP-NOW 协议（内置）

#### 后端（Rust）
- axum 0.8（Web 框架）
- tokio 1（异步运行时）
- serialport 4.3（串口通信）
- serde 1.0（序列化）
- base64 0.22（编码）
- chrono 0.4（时间处理）
- tracing 0.1（日志）
- anyhow 1.0（错误处理）
- dotenvy 0.15（环境变量）

#### 前端（Node.js/Bun）
- vue 3.5.35
- vite 8.0.16
- tailwindcss 4
- @tailwindcss/vite
- typescript 5.4.5

### Known Issues
- 视频传输在高分辨率下可能卡顿（建议降低分辨率或帧率）
- ESP-NOW 通信距离受环境影响（建议空旷场地使用）

### Future Plans
- 增加第二路摄像头支持
- 添加传感器数据（超声波、红外）
- 实现路径记录与回放功能
- 添加自动避障算法
- 支持手机端控制界面
- 添加电池电量监测

---

[Unreleased]: https://github.com/halfoffive/esp-smart-car/compare/v1.8.0...HEAD
[1.8.0]: https://github.com/halfoffive/esp-smart-car/compare/v1.7.4...v1.8.0
[1.7.4]: https://github.com/halfoffive/esp-smart-car/compare/v1.3.0...v1.7.4
[1.3.0]: https://github.com/halfoffive/esp-smart-car/compare/v1.2.2...v1.3.0
[1.2.2]: https://github.com/halfoffive/esp-smart-car/compare/v1.2.1...v1.2.2
[1.2.1]: https://github.com/halfoffive/esp-smart-car/compare/v1.2.0...v1.2.1
[1.2.0]: https://github.com/halfoffive/esp-smart-car/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/halfoffive/esp-smart-car/releases/tag/v1.1.0
[1.0.0]: https://github.com/halfoffive/esp-smart-car/releases/tag/v1.0.0
