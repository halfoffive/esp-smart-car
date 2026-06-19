# 嵌入式固件 - Knowledge Base

**Location:** `firmware/`
**Language:** Arduino/C++ (ESP32)
**IDE:** Arduino IDE

## Structure

```
firmware/
├── libraries/
│   └── wireless_protocol/   # Arduino 库：ESP-NOW 通信协议
│       └── src/
│           └── wireless.h   # 共享头文件（WirelessPacket、OdometryPacket、VideoPacket）
├── car_controller/          # 车载 ESP32-S3（Freenove FNK0085 单芯片架构）
│   ├── car_controller.ino   # 主程序（摄像头采集 + 电机控制 + ESP-NOW 直发 + BLE 广播）
│   ├── motor_control.h      # 函数式电机控制（L298N 驱动 4 个电机）
│   ├── odometer.h           # 编码器测速模块（GPIO 1/2 中断）
│   ├── pid_control.h        # PID控制器（直线修正 + 航向锁定）
│   ├── camera_config.h      # OV2640 摄像头配置（ESP32-S3 CAM 标准引脚）
│   └── video_stream.h       # 视频流传输（ESP-NOW 分包直发接收器）
└── receiver_dongle/         # 接收器 ESP32-C6（USB dongle）
    └── receiver_dongle.ino  # USB桥接 + 测速转发 + BLE 扫描 + 链路状态上报
```

## Where to Look

| Task | Location | Notes |
|------|----------|-------|
| 修改电机控制 | `car_controller/motor_control.h` | 纯函数，差速控制 |
| 修改无线协议 | `libraries/wireless_protocol/src/wireless.h` | 12字节数据包 + 测速包 + 视频包 |
| 修改测速模块 | `car_controller/odometer.h` | 编码器中断+速度计算 |
| 修改PID控制 | `car_controller/pid_control.h` | 直线修正+航向锁定 |
| 修改摄像头配置 | `car_controller/camera_config.h` | 分辨率/质量/引脚 |
| 修改视频传输 | `car_controller/video_stream.h` | ESP-NOW 分包传输（128字节/包） |
| 修改接收器逻辑 | `receiver_dongle/receiver_dongle.ino` | USB桥接+测速转发+BLE扫描+链路状态 |
| 修改车载主循环 | `car_controller/car_controller.ino` | 视频采集+电机控制+ESP-NOW收发+BLE广播 |

## Code Map

| Symbol | Type | Location | Role |
|--------|------|----------|------|
| `MotorState` | struct | `motor_control.h` | 单个电机状态 |
| `VehicleMotion` | struct | `motor_control.h` | 整车运动状态 |
| `MotorDirection` | enum class | `motor_control.h` | 方向枚举 |
| `WirelessPacket` | struct | `wireless.h` | 通信数据包（12字节） |
| `OdometryPacket` | struct | `wireless.h` | 测速数据包 |
| `VideoPacket` | struct | `wireless.h` | 视频分包（最大139字节） |
| `EncoderConfig` | struct | `odometer.h` | 编码器配置 |
| `WheelSpeed` | struct | `odometer.h` | 单轮速度数据 |
| `OdometryData` | struct | `odometer.h` | 整车测速数据 |
| `PIDParams` | struct | `pid_control.h` | PID参数 |
| `PIDState` | struct | `pid_control.h` | PID计算状态 |
| `SmartMotorOutput` | struct | `pid_control.h` | 智能电机输出 |
| `DriveMode` | enum class | `pid_control.h` | 行走模式（NORMAL/STRAIGHT_LINE/HEADING_LOCK） |
| `CameraConfiguration` | struct | `camera_config.h` | 摄像头配置 |
| `FrameState` | struct | `video_stream.h` | 视频帧状态 |
| `StreamState` | struct | `video_stream.h` | 传输状态 |

## Conventions

- **函数式编程**：所有数据用 `const`，状态通过函数返回值传递
- **中文注释**：所有函数、结构体、枚举都有详细中文注释
- **命名空间**：每个模块独立命名空间（`PinConfig`, `OdometerConfig`, `PIDDefaults` 等）
- **不可变状态**：`const struct` 存储状态，新状态通过函数创建
- **硬件分离**：纯函数处理逻辑，`apply_*` 函数处理硬件副作用
- **inline 函数**：头文件中所有函数使用 `inline` 避免多重定义

## Anti-Patterns

- **禁止使用全局可变状态**：所有状态必须显式传递（ISR 计数器等硬件相关变量除外）
- **禁止隐式转换**：`enum class` 替代 `enum`
- **禁止混合关注点**：电机、测速、PID、无线、摄像头逻辑完全分离
- **禁止空 catch 块**：所有错误必须处理
- **禁止更改调试日志级别**：`car_controller.ino` 中 `DEBUG_*` 宏由用户控制，AI 不得擅自修改

## Commands

```bash
# Arduino IDE 上传
# 1. 打开 .ino 文件
# 2. 选择工具 -> 开发板 -> ESP32S3 Dev Module（车载）/ ESP32C6 Dev Module（接收器）
# 3. 选择端口
# 4. 点击上传
```

## Notes

- **ESP-NOW 信道**：固定 channel 1，所有设备必须一致
- **MAC 地址**：在 `libraries/wireless_protocol/src/wireless.h` 中配置，支持运行时动态修改（`setTargetCarMac`）
- **PWM 频率**：电机 1kHz（`analogWrite` 默认）
- **内存**：接收器视频帧缓冲 32768 字节（`ReceiverConfig::BUFFER_SIZE`），注意不要溢出
- **电源**：电机和逻辑电源必须隔离，共地
- **单芯片架构**：车载 ESP32-S3 同时承担摄像头采集 + 电机控制 + 编码器测速 + PID + ESP-NOW 收发 + BLE 广播
- **视频传输**：ESP32-S3 通过 ESP-NOW 直发视频帧到接收器（无 Serial1 桥接），分包大小 128 字节
- **BLE 广播**：车载端广播 Manufacturer Data（Company ID 0xFFFF + WiFi MAC 6字节），接收器扫描时提取 WiFi MAC 用于 ESP-NOW 配对
- **链路状态**：接收器收到 'P' 命令或每 5 秒主动上报 `{"t":"link",...}` JSON，后端解析后推送前端
- **测速上报**：车载端每 200ms 通过 ESP-NOW 发送 `OdometryPacket`，接收器转为 JSON 转发到 PC
