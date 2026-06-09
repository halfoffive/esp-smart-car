# ESP32 智能车控制系统

基于 ESP32 的智能车控制系统，包含嵌入式固件、无线通信、视频传输和 Web 控制界面。

## 项目结构

```
esp-smart-car/
├── firmware/                    # 嵌入式固件
│   ├── car_controller/          # 车载控制器（ESP32-C6）
│   │   ├── motor_control.h      # 电机控制（函数式编程，差速支持）
│   │   ├── servo_control.h      # 舵机控制（函数式编程）
│   │   ├── wireless.h           # 无线通信（ESP-NOW，含测速协议）
│   │   ├── odometer.h           # 编码器测速模块
│   │   ├── pid_control.h        # PID控制器（直线修正+航向锁定）
│   │   └── car_controller.ino   # 主程序
│   ├── camera_module/           # 摄像头模块（ESP32-S3 CAM）
│   │   ├── camera_config.h      # 摄像头配置
│   │   ├── video_stream.h       # 视频流传输
│   │   └── camera_module.ino    # 主程序
│   └── receiver_dongle/         # 电脑端接收器（ESP32-C6）
│       └── receiver_dongle.ino  # 主程序（含测速数据转发）
├── desktop/                     # 桌面端控制界面
│   ├── backend/                 # Rust 后端
│   │   ├── Cargo.toml
│   │   └── src/
│   │       ├── main.rs          # 主程序
│   │       ├── serial.rs        # 串口通信（含测速数据解析）
│   │       ├── websocket.rs     # WebSocket 处理（含测速广播）
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
│               └── useStatus.ts
└── docs/                        # 文档
    └── hardware.md              # 硬件接线说明
```

## 硬件需求

### 主控板
- **ESP32-C6 开发板** x2
  - 1个：车载控制器（连接电机、舵机）
  - 1个：电脑端接收器（USB 连接电脑）
  
- **ESP32-S3 CAM** x2
  - 摄像头模块（视频传输）
  - 可选：第二路摄像头

### 驱动模块
- **L298N 电机驱动模块** x2
  - 控制 4 个直流电机
  - 支持正转、反转、停止
  - PWM 调速

- **SG90 舵机** x2
  - 水平舵机：摄像头左右旋转
  - 垂直舵机：摄像头上下旋转

- **霍尔编码器/红外编码器** x2
  - 左轮编码器：每圈20脉冲
  - 右轮编码器：每圈20脉冲
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

#### 数据包格式
```
[魔术字 1字节] [版本 1字节] [类型 1字节] [数据 1字节] [速度 1字节] [序列号 2字节] [校验和 1字节]
```

#### 视频帧格式
```
[帧头 0xAA 0x55] [帧大小 4字节] [帧数据 N字节]
```

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
| U | 云台上 | - |
| J | 云台下 | - |
| H | 云台左 | - |
| K | 云台右 | - |
| C | 云台居中 | - |
| M | 普通模式（无修正） | mode=0 |
| L | 直线修正模式 | mode=1 |
| B | 航向锁定模式 | mode=2 |

## 安装说明

### 嵌入式固件

1. 安装 Arduino IDE
2. 添加 ESP32 开发板支持
   - 文件 -> 首选项 -> 附加开发板管理器网址
   - 添加：`https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. 安装库：
   - ESP32Camera
   - ESP-NOW
4. 选择开发板：
   - ESP32-C6："ESP32C6 Dev Module"
   - ESP32-S3："ESP32S3 Dev Module"
5. 上传固件

### 桌面端

前端已集成到后端中，构建一次后启动后端即可直接访问 Web UI。

#### 后端（Rust）
```bash
cd desktop/backend

# 编译后端（自动构建前端，设置 SKIP_FRONTEND_BUILD=1 可跳过）
cargo build

# 运行（自动提供前端页面，访问 http://localhost:8080）
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

1. 启动车载控制器（ESP32-C6）
2. 启动摄像头模块（ESP32-S3 CAM）
3. 连接电脑端接收器（ESP32-C6）到电脑 USB
4. 启动 Rust 后端（自动提供前端页面）
5. 在浏览器中打开 `http://localhost:8080`
6. 在 Web UI 中连接串口

## 开发说明

### 函数式编程风格

嵌入式固件采用函数式编程风格：
- 数据不可变（使用 `const`）
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
- 云台：方向键或控制面板

## 测试

```bash
cd desktop/backend
cargo test         # 运行所有 35 个 Rust 测试（无需硬件连接）
cargo clippy       # 静态分析检查
```

## 故障排除

### 无线通信失败
- 检查 MAC 地址配置
- 确认信道一致
- 检查距离和干扰

### 视频传输卡顿
- 降低分辨率
- 降低帧率
- 检查 WiFi 信号

### 电机不转
- 检查电源电压
- 检查 L298N 接线
- 检查 PWM 信号

## 版本历史

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
