# ESP32 智能车控制系统

*部分内容由 AI 生成*

基于 ESP32 的智能车控制系统，包含嵌入式固件、无线通信、视频传输和 Web 控制界面。

**架构**：ESP32-S3 单芯片车载控制器（Freenove FNK0085）+ ESP32-C6 接收器 Dongle，S3 同时承担摄像头采集、电机控制、编码器测速、PID、ESP-NOW 收发、BLE 广播全部车载功能。

## 项目结构

```
esp-smart-car/
├── firmware/                    # 嵌入式固件
│   ├── libraries/               # Arduino 库（跨 sketch 共享）
│   │   └── wireless_protocol/   # 无线通信协议库（ESP-NOW + 视频分包）
│   │       └── src/
│   │           └── wireless.h   # 共享头文件
│   ├── car_controller/          # 车载控制器（ESP32-S3，Freenove FNK0085）
│   │   ├── motor_control.h      # 电机控制（函数式编程，差速支持）
│   │   ├── odometer.h           # 编码器测速模块
│   │   ├── pid_control.h        # PID控制器（直线修正+航向锁定）
│   │   ├── camera_config.h      # 摄像头配置（OV2640）
│   │   ├── video_stream.h       # 视频流传输（ESP-NOW 直发）
│   │   └── car_controller.ino   # 主程序
│   └── receiver_dongle/         # 电脑端接收器（ESP32-C6）
│       └── receiver_dongle.ino  # 主程序（含测速数据转发、BLE 扫描、链路状态上报）
├── desktop/                     # 桌面端控制界面
│   ├── backend/                 # Rust 后端
│   │   ├── Cargo.toml
│   │   └── src/
│   │       ├── main.rs          # 主程序
│   │       ├── serial.rs        # 串口通信（含测速/链路状态解析）
│   │       ├── websocket.rs     # WebSocket 处理（含测速/链路状态广播）
│   │       └── api.rs           # HTTP API
│   └── frontend/                # Vue 前端
│       ├── package.json
│       ├── vite.config.ts
│       └── src/
│           ├── App.vue
│           ├── main.ts
│           ├── style.css
│           ├── components/
│           │   ├── VideoPlayer.vue
│           │   ├── ControlPanel.vue
│           │   ├── StatusBar.vue
│           │   └── SpeedDashboard.vue
│           └── composables/
│               ├── useWebSocket.ts
│               ├── useKeyboard.ts
│               ├── useApi.ts
│               ├── useStatus.ts
│               └── useBackendHealth.ts
└── docs/                        # 文档
    └── hardware.md              # 硬件接线说明
```

## 硬件需求

### 主控板
- **ESP32-S3 开发板**（Freenove FNK0085）x1
  - 车载控制器：单芯片承担摄像头采集 + 电机控制 + 编码器测速 + PID + ESP-NOW 收发 + BLE 广播

- **ESP32-C6 开发板** x1
  - 电脑端接收器（USB 连接电脑，支持 BLE 扫描 + 链路状态上报）

### 驱动模块
- **L298N 电机驱动模块** x2
  - 控制 2 路电机（左/右各一路，每路并联驱动 2 个直流电机）
  - 支持正转、反转、停止
  - PWM 调速

- **光电编码器（光栅码盘 + 光敏元件）** x2
  - 左轮编码器：每圈20脉冲
  - 右轮编码器：每圈20脉冲
  - 下降沿（FALLING）触发中断
  - 用于测速和直线修正

### 电机
- **直流减速电机** x4
  - 2个左侧（并联）
  - 2个右侧（并联）

### 电源
- **7.4V 锂电池**（推荐）
- **5V 稳压模块**（给 ESP32 供电）

## 软件架构

### 通信协议

#### ESP-NOW（无线通信）
- 协议：2.4GHz WiFi
- 模式：无连接广播
- 延迟：< 10ms
- 距离：~100m
- 用途：车载 S3 ↔ 接收器 C6 双向通信（视频帧、命令、测速、状态）

#### BLE 扫描
- 接收器支持 BLE 设备扫描
- 通过 'B' 命令触发扫描
- 车载 S3 BLE 广播嵌入 WiFi MAC（Manufacturer Data）
- 扫描结果通过 WebSocket 推送到前端

#### 链路状态探测
- 接收器收到 'P' 命令时立即上报 `{"t":"link",...}` JSON
- 接收器每 5 秒主动上报一次链路状态（周期性心跳）
- 后端 `connect_serial` 成功后发送 'P' 探测命令
- 前端显示 4 级链路状态：探测中 → Dongle 已连接 → 车载已配对 → 车载在线

#### 数据包格式
```
[魔术字 1字节] [版本 1字节] [类型 1字节] [数据 1字节] [速度 1字节] [序列号 2字节] [校验和 1字节]
```

#### 视频帧格式
```
[帧头 0xAA 0x55] [帧大小 4字节] [帧数据 N字节]
```

S3 单芯片直接 ESP-NOW 分包发送视频帧到接收器（每包 128 字节），不再经过 Serial1 桥接。

### 命令类型

| 命令 | 说明 | 数据 |
|------|------|------|
| W | 前进 | - |
| A | 左转 | - |
| S | 后退 | - |
| D | 右转 | - |
| Q | 原地左转 | - |
| E | 原地右转 | - |
| 空格 | 停止 | - |
| 1-9 | 速度设置 | 1-9 |
| T | 行走模式切换 | 1字节模式值(0/1/2) |
| B | BLE 扫描 | - |
| P | 链路状态探测 | - |

## 安装说明

### 嵌入式固件

1. 安装 Arduino IDE
2. 添加 ESP32 开发板支持
   - 文件 -> 首选项 -> 附加开发板管理器网址
   - 添加：`https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. 安装无线协议库（`wireless_protocol`）：
   - 方式A（推荐）：将 Arduino IDE 的 sketchbook 路径设为 `firmware/`，库会自动识别
   - 方式B：将 `firmware/libraries/wireless_protocol` 复制到 Arduino 的库文件夹（`~/Arduino/libraries/` 或 `%USERPROFILE%\Documents\Arduino\libraries\`）
4. 安装库：
   - ESP32Camera
   - ESP-NOW
5. 选择开发板：
   - 车载控制器（ESP32-S3，Freenove FNK0085）："ESP32S3 Dev Module"
   - 接收器 Dongle（ESP32-C6）："ESP32C6 Dev Module"
6. 上传固件

### 桌面端

前端已集成到后端中，构建一次后启动后端即可直接访问 Web UI。

#### 后端（Rust）
```bash
cd desktop/backend

# 编译后端（自动构建前端并嵌入二进制，设置 SKIP_FRONTEND_BUILD=1 可跳过）
cargo build

# 运行（前端已编译进 exe，无需 .env 文件即可在任意位置运行，访问 http://localhost:8080）
cargo run
```

#### 前端（Vue）- 开发调试
```bash
cd desktop/frontend

# 使用 bun 安装依赖
bun install

# 开发模式（独立前端开发服务器，端口 3000，代理 API 到后端）
bun run dev

# 构建（产物输出到 desktop/backend/frontend/dist，供后端集成）
bun run build
```

## 启动顺序

1. 启动车载控制器（ESP32-S3，Freenove FNK0085）— 单芯片承担摄像头 + 电机 + 编码器 + ESP-NOW + BLE
2. 连接电脑端接收器（ESP32-C6）到电脑 USB
3. 启动 Rust 后端（自动提供前端页面）
4. 在浏览器中打开 `http://localhost:8080`
5. 前端自动探测后端可用性，不可用时显示红色横幅
6. 在 Web UI 中连接串口（串口列表会自动通过 WebSocket 实时推送，连接后自动发送 'P' 探测链路状态）

## 开发说明

### 函数式编程风格

嵌入式固件采用函数式编程风格：
- 数据不可变（通过值传递新对象，不修改旧对象）
- 纯函数（无副作用）
- 显式状态传递
- 高阶函数组合

```cpp
// 纯函数：创建运动状态
VehicleMotion createForwardState(const uint8_t speed) {
    return VehicleMotion(...);
}

// 纯函数：解析命令
VehicleMotion parseWASDCommand(const char cmd, const uint8_t speed) {
    // 无状态，无副作用
}

// 应用状态（副作用明确）
void applyVehicleMotion(const VehicleMotion& motion) {
    // 硬件操作
}
```

### Web UI 控制

- 键盘：WASD 控制方向
- 鼠标：点击控制面板按钮
- 速度：数字键 1-9 或滑块
- BLE：扫描周围蓝牙设备

## 测试

```bash
cd desktop/backend
cargo test         # 运行所有 58 个 Rust 测试（无需硬件连接）
cargo clippy       # 静态分析检查
```

## 故障排除

### 无线通信失败
- 检查 MAC 地址配置（BLE 扫描复制 WiFi MAC，不是 BLE MAC）
- 确认信道一致（固定信道 1）
- 检查距离和干扰

### 链路状态异常
- 串口连接后前端应显示 4 级链路状态：探测中 → Dongle 已连接 → 车载已配对 → 车载在线
- 若停留在"探测中"：检查 Dongle 固件是否支持 'P' 命令
- 若停留在"Dongle 已连接"：检查车载 S3 是否启动、ESP-NOW 配对是否成功
- 若停留在"车载已配对"：检查车载 S3 是否在发送数据（测速/视频帧）

### 后端不可用
- 前端启动时自动探测 `/api/status`（1 秒超时）
- 不可用时顶部显示红色横幅，所有控制 UI 禁用
- 检查后端 exe 是否运行、端口 8080 是否占用

### 视频传输卡顿
- 降低分辨率
- 降低帧率
- 检查 ESP-NOW 信号质量（距离、干扰）

### 电机不转
- 检查电源电压
- 检查 L298N 接线（GPIO 38/39/40 左侧，41/42/21 右侧）
- 检查 PWM 信号

### 紧急停止无响应
- 紧急停止改为长按 500ms 触发（防误触），单击无效
- 检查 WebSocket 连接是否正常

## 版本历史

- v1.3.1 - 2026-06-19（未发布）
  - 后端并发安全：`MutexExt` 锁污染自动恢复、串口 generation 防竞态、`video_task` 可安全取消 + 心跳超时
  - 前端：WebSocket 重入/心跳/连接超时、ControlPanel 状态回滚与错误提示、useApi 超时与 JSON 解析、定时器清理、标签页隐藏停车
  - 固件：命令超时自动停止常量 `COMMAND_TIMEOUT_MS`、视频包越界修复、receiver_dongle `dataLen` 校验
  - 固件双电机简化：`motor_control.h` `VehicleMotion` 从 4 字段（frontLeft/frontRight/rearLeft/rearRight）改为 2 字段（left/right）
  - 固件编码器修复：ISR 定义从 `odometer.h` 移至 `car_controller.ino`（非 `inline`），修复 `dangerous relocation` 链接错误；触发沿 `RISING`→`FALLING`，对齐参考博客光电编码器设计
  - 已知：当前 Windows + Rust 1.96.0 环境下 `cargo build` 因工具链 `Command::output` 返回 `Os { code: 0 }` 失败，与本项目代码无关，详见 `%TEMP%\rust_panic_report.md`

- v1.9.1 - 2026-06-18
  - **S3 平台整合与可观测性增强**：砍掉车载 ESP32-C6，由 ESP32-S3（Freenove FNK0085）单芯片承担全部车载功能（摄像头 + 电机 + 编码器 + PID + ESP-NOW + BLE）
  - 固件：car_controller 目标板 C6→S3，合并 camera_module 代码，删除 Serial1 桥接，启用 ESP-NOW 视频直发，删除 servo_control.h；motor_control 引脚改 GPIO 38/39/40/41/42/21；odometer 编码器引脚改 GPIO 1/2；receiver_dongle 新增 'P' 命令链路状态上报 + 5 秒周期心跳
  - 后端：新增 LinkStatus 结构体、链路状态解析、首帧/周期摘要/命令转发/链路状态日志、read_next 返回 Vec、video_task 共享哈希、get_ble_devices 补 wifi_mac、status WS 推送
  - 前端：新增 useBackendHealth 后端健康检测、4 级链路状态 UI、useStatus 移除轮询改 WS 推送、WS_URL 子路径支持、紧急停止长按 500ms、useKeyboard select 检查、版本 v1.3.0
  - Breaking: 车载控制器目标板 C6→S3；firmware/camera_module/ 目录删除；前端 /api/status 轮询移除改 WS 推送；紧急停止改长按 500ms
  - 验证: cargo clippy 0 warnings；cargo test 58 测试全过；bun run build 成功

- v1.8.1 - 2026-06-13
  - 修复 camera_module.ino Serial1.begin 参数错误（`SERIAL_8N1` 被误作 `rxPin`）
  - 修复 receiver_dongle.ino BLEDevice::init 重复调用（改为 setup() 中初始化一次）
  - 修复 car_controller.ino 视频帧接收/转发竞态（帧就绪时暂停接收）

- v1.8.0 - 2026-06-13
  - 硬件重构：移除 SG90 舵机（云台），ESP32-S3 与 C6 改为软串口直连（GPIO 14/15），接收器新增 BLE 扫描
  - 固件：car_controller 移除舵机代码新增软串口视频帧接收/转发，camera_module 改为 Serial1 发送，receiver_dongle 新增 BLE 扫描
  - 后端：新增 BleDevice 数据结构、GET /api/ble-devices 端点、ble_scan WebSocket 消息
  - 前端：移除云台/MAC UI，新增 BLE 设备扫描 UI
  - Breaking: CommandType::SERVO 和 DeviceRole::CAMERA 从无线协议中移除，云台命令不再有效

- v1.7.4 - 2026-06-13
  - 综合代码审计 v9：修复 36 项问题（1 P1 + 4 P2 + 2 Serious + 3 High + 26 P3/Low）
  - P1: 摄像头 SERVO 转发修复（camera_module.ino — 移除损坏的 sendToCar）
  - P2: 航向锁定角度环绕归一化、MAC 设置局部故障修复、autoCalibrate 除零保护、帧头 JPEG SOI 验证
  - Serious: useStatus 孤立轮询器逃逸修复、速度发送改用 sendSpeed 协议
  - High: 串口连接 WS 失败回滚、键盘焦点/IME 检查
  - 固件 P3: 死代码清理、返回值检查、include 路径规范化、const_cast 消除、ServoConfig 去重
  - 后端优化: 帧哈希改用 DefaultHasher、chrono→std::time、tower→dev-deps、line_buf 硬上限
  - 前端优化: pinia 移除、死 CSS 清理、运行时间显示秒数、ARIA 属性去重
  - 文档: 5 个 AGENTS.md 同步更新（AppState 位置、视频缓冲区大小、composables 列表等）

- v1.7.3 - 2026-06-13
  - 综合代码审计 v8：修复 9 项问题（2 P2 + 7 P3）
  - P2: drive_mode 命令原子性修复（websocket.rs `send_bytes` 替代两次 `send_command`）
  - P2: ControlPanel WebSocket 连接异常处理（wsConnect 添加 await + try-catch）
  - P3: 固件死代码清理（5项：g_targetHeading、changeMotorState、g_isStreaming、TIMEOUT_MS、WirelessState 死字段）
  - P3: 视频流配置注释修正（JPEG_QUALITY_MAX/MIN 语义对齐 ESP32 驱动）
  - P3: 前端 FPS 去重（VideoPlayer 统一使用 useWebSocket().videoFps）
  - P3: StatusBar 回退逻辑修正（|| 5 → 显式 null/undefined 检查）
  - P3: .env 死配置清理（移除未被读取的 VIDEO_FRAME_BUFFER_SIZE / MAX_VIDEO_PACKET_SIZE）
  - P3: receiver_dongle.ino odometry JSON %u 格式符添加 static_cast 显式转换

- v1.7.2 - 2026-06-13
  - 综合代码审计 v5.3：修复 2 项问题（1 P1 + 1 P3）
  - P1: 视频包校验和验证（接收端添加校验和检查，防止花屏）
  - P3: 箭头键临时数组优化（改为 const Set）

- v1.7.1 - 2026-06-13
  - 综合代码审计 v5.2：修复 3 项问题（2 P2 + 1 P3）
  - P2: 帧捕获错误恢复机制（连续失败 10 次自动重启摄像头）
  - P2: 非标准无线包日志（便于调试）
  - P3: 串口任务重启指数退避（3s→60s 最大）

- v1.7.0 - 2026-06-13
  - 综合代码审计 v5：修复 68 项问题（6 P0 + 15 P1 + 8 P2 + 5 P3）
  - P0: ESP-NOW 网络拓扑修复（Receiver 同时添加 Car 和 Camera Peer）
  - P0: DRIVE_MODE 协议重构（专属命令字节 'T'，消除与 MAC_CONFIG 的 'M' 冲突）
  - P0: 串口数据流解析器重构（BufReader + 统一缓冲状态机，修复帧头重叠和数据互斥吞没）
  - P0: 串口连接自动触发 WebSocket 连接
  - P0: 视频缓冲区从 4KB 扩大到 32KB
  - P0: 紧急停止改为仅运动命令显式解除
  - P1: HEADING_LOCK 航向锁定模式实现（航向 PID 控制）
  - P1: MAC 配对表运行时更新、MAC 配置帧边界防护
  - P1: 串口任务退出后自动重启、dotenv 时序修复、命令失败感知
  - P2: Base64 共享编码、/api/ports 缓存、增量平均速度、移除 Pinia 死代码
  - P3: appearance 修复、箭头键修复、duty 溢出修复、volatile 清理

- v1.6.0 - 2026-06-12
  - 自动串口扫描：后端每秒扫描可用串口，变化时通过 WebSocket 主动推送给前端
  - MAC 地址动态配置：前端可输入车载 ESP32-C6 的 MAC 地址，通过 WebSocket 下发到接收器，支持 `localStorage` 持久化
  - 固件支持运行时修改目标 MAC 地址，`wireless.h` 中 MAC 数组从 `constexpr` 改为可变
  - 后端新增 7 个测试，总计 43 个测试全部通过

- v1.5.2 - 2026-06-12
  - 添加串口扫描功能：ControlPanel.vue 新增"扫描"按钮，调用 `GET /api/ports` 获取可用串口列表，页面加载时自动扫描

- v1.5.1 - 2026-06-12
  - 修复 api.rs `connect_serial` 中 `port_name` 所有权错误（E0382），闭包前添加 clone

- v1.5.0 - 2026-06-09
  - P0 固件编译错误修复：重构 `wireless.h` 为 Arduino 库（`firmware/libraries/wireless_protocol/`），避免复制到各 sketch 目录
  - 修复 ESP32 Arduino core 3.3.8 回调签名不兼容（`esp_now_send_cb_t` / `esp_now_recv_cb_t`）
  - 修复所有状态结构体 `const` 成员导致的 "use of deleted function 'operator='" 编译错误
  - 修复 `car_controller.ino` 中 `onDataRecv` 重定义问题
  - 修复 `odometer.h` C++20 `volatile` 弃用警告
  - 将 `VideoPacket` 和 `StreamConfig` 定义迁移到 `wireless.h`，供 `receiver_dongle.ino` 共享

- v1.4.0 - 2026-06-09
  - 全面代码排查与优化 v3（27项修复，启用 karpathy-guidelines + frontend-design）
  - 前端：修复 onUnmounted 未 await、RAF 空转、命令静默丢弃、WebSocket 旧连接清理不安全、handleSpeedInput 重复
  - 固件：修复云台命令字符不匹配（'L'/'R'→'H'/'K'）、OdometryPacket 强转、VideoPacket 冗余数据、dataLen 边界检查、OdometryPacket 重复处理
  - 后端：修复 from_utf8_lossy 数据丢失、connect_serial 锁持有过长、帧头查找无超时、帧大小上限过高、video_frame Arc 共享
  - 固件：odometer.h volatile、pid_control.h dt==0、sendOdometryData 溢出、motor_control.h speed/2 对称、wireless.h const_cast
  - 后端：build.rs bun install 条件判断、Cargo.toml rust-version

- v1.3.2 - 2026-06-09
  - 全面代码排查与优化 v2（23项修复）
  - 固件：修复电机引脚错误（GPIO 10-13→4-8）、云台角度下溢、摄像头引脚类型、JPEG质量枚举反转、智能修正初始值、'D'键冲突、超时计时器遗漏
  - 前端：修复串口/WS状态混淆、运动网格底行重复、按键高亮不工作、鼠标离开不停车、定时器类型混淆、WebSocket旧连接泄漏、useApi状态码检查
  - 后端：修复帧哈希碰撞丢帧、frame_buffer泄漏、REST API速度不同步、connect失败状态残留、Mutex类型统一、odometry广播节流
  - 新增 useStatus.ts composable 合并重复轮询
  - WebSocket重连改为指数退避、WS_URL动态构建

- v1.3.1 - 2026-06-09
  - 修复 motor_control.h GPIO 引脚错误（GPIO 10-13 在 ESP32-C6 上连接 SPI Flash，改为 GPIO 4-8）
  - 修复 servo_control.h 云台角度 uint8_t 下溢（0° - 5 = 251°）
  - 修复 camera_config.h PWDN/RESET 引脚类型（uint8_t → int8_t，确保 -1 正确存储）
  - 修复 camera_config.h JPEG 质量枚举反转（ESP32 驱动中数值越小质量越高）
  - 修复 car_controller.ino 智能修正初始值（setup() 不再覆盖为 true）
  - 修复 car_controller.ino 行走模式切换时双标志不同步
  - 修复 receiver_dongle.ino 'D' 键冲突（云台下命令被 MOVE 分支截获）
  - 修复 receiver_dongle.ino Serial.read() 类型（char → int，避免符号扩展）
  - 修复 car_controller.ino 速度/云台命令未更新超时时间戳

- v1.3.0 - 2026-06-08
  - 全面代码排查与优化（前端+后端+固件）
  - 新增 useApi.ts composable、ARIA 无障碍标签、固件调试开关
  - useWebSocket/useKeyboard 重构，websocket.rs CancellationToken 优雅关闭
  - 修复内存泄漏、中断安全、类型安全等问题
  - 新增 SKIP_FRONTEND_BUILD 环境变量
  - 自动化测试增至 35 个

- v1.2.2 - 2026-06-08
  - 修复 serial.rs 阻塞 I/O 在 async 上下文问题（`run_serial_task` 改用 `tokio::task::spawn_blocking()`）
  - 优化 serial.rs 锁持有时间（`serial_manager` 改用 `std::sync::Mutex`，读取数据后立即释放）
  - 移除 serial.rs `read_line` 冗余 `line_buffer.clear()`

- v1.2.1 - 2026-06-08
  - 修复 api.rs 空命令发送 0x00 问题（改为返回 400 Bad Request）
  - 重构 api.rs StatusResponse 构造（DRY，消除三段重复代码）
  - 优化 api.rs `get_status` 锁争用（逐把加锁释放，减少同时持有）
  - 统一 websocket.rs `handle_message` 锁顺序与 `get_status` 一致
  - 修复 websocket.rs drive_mode 分支重复加锁问题
  - 修复 websocket.rs drive_mode 2 错误指令字符（'H' 改为 'B'，航向锁定模式）
  - 重构 useWebSocket.ts 生命周期（单管理员模式，防止多组件卸载断连）
  - 修复 useWebSocket.ts 重连竞争（添加 shouldReconnect flag）
  - 修复 VideoPlayer.vue RAF 内存泄漏（添加 onUnmounted 取消 requestAnimationFrame）
  - 修复 VideoPlayer.vue FPS 计算初始值不准确（lastFpsUpdate 从 0 改为 Date.now()）
  - 修复 ControlPanel.vue 云台指令（左 'L'→'H'，右 'R'→'K'）
  - 修复 ControlPanel.vue smartDriveOn 初始值（true→false，匹配固件默认）
  - 添加 ControlPanel.vue 速度滑块 200ms 防抖
  - 修复 StatusBar.vue 连接状态（isConnected 从本地 ref 改为 useWebSocket 导入）
  - 修复 useWebSocket.ts 类型安全（odometry 解析运行时校验替代 as number）
  - 修复 useWebSocket.ts 错误处理（sendCommand 添加 try-catch）
  - 移除 VideoPlayer.vue 录制空操作按钮（无实际录制逻辑）

- v1.2.0 - 2026-06-07
  - 前端依赖大版本升级：TailwindCSS v3 → v4，Vite 5 → 8，Vue 3.4 → 3.5.35
  - 修复滑块 thumb 垂直对齐
  - 移除 tailwind.config.js、postcss.config.js（迁移到 CSS @theme）
  - 新增 Rust 自动化测试（25 个测试用例）

- v1.1.3 - 2026-06-07
  - 修复滑块 thumb 垂直对齐（`margin-top: -6px`）
  - Rust `cargo build` 自动构建前端（新增 `build.rs`）

- v1.1.2 - 2026-06-07
  - 速度滑块改为无极调节（step 0.1），移除快速按钮

- v1.1.1 - 2026-06-07
  - 修复速度显示异常（`current_speed` 初始值从 128 改为 5）
  - 修复速度滑块与快速按钮视觉对齐
  - SpeedDashboard 改用 WebSocket odometry 实时数据

- v1.1.0 - 2026-06-07
  - 升级 axum 0.8 / tower 0.5
  - 重构 WebSocket 为 mpsc channel 模式
  - 修复编译警告和 uptime 计算

- v1.0.0 - 初始版本
  - 基础运动控制
  - 视频传输
  - Web UI
  - ESP-NOW 通信

## 许可证

MIT License

## 作者

智能车项目团队
