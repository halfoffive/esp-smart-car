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
│       ├── tailwind.config.js
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
│               └── useKeyboard.ts
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

# 编译后端
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
