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
│           └── wireless.h   # 共享头文件（WirelessPacket、OdometryPacket）
├── car_controller/          # 车载 ESP32-C6
│   ├── motor_control.h      # 函数式电机控制
│   ├── odometer.h           # 编码器测速模块
│   ├── pid_control.h        # PID控制器（直线修正+航向锁定）
│   └── car_controller.ino   # 主程序（含 HardwareSerial 视频帧接收/转发）
├── camera_module/          # 摄像头 ESP32-S3 CAM
│   ├── camera_config.h      # OV2640 配置
│   ├── video_stream.h       # 视频流传输（历史：原 ESP-NOW 分包，当前 Serial1 直接发送）
│   └── camera_module.ino    # 主程序（Serial1 发送视频帧）
└── receiver_dongle/          # 接收器 ESP32-C6
    └── receiver_dongle.ino  # USB桥接 + 测速数据转发 + BLE 扫描
```

## Where to Look

| Task | Location | Notes |
|------|----------|-------|
| 修改电机控制 | `car_controller/motor_control.h` | 纯函数，差速控制 |
| 修改无线协议 | `libraries/wireless_protocol/src/wireless.h` | 12字节数据包 + 测速包 |
| 修改测速模块 | `car_controller/odometer.h` | 编码器中断+速度计算 |
| 修改PID控制 | `car_controller/pid_control.h` | 直线修正+航向锁定 |
| 修改摄像头配置 | `camera_module/camera_config.h` | 分辨率/质量 |
| 修改视频传输 | `camera_module/video_stream.h` | 历史：帧分包传输；当前通过 Serial1 直接发送完整帧 |
| 修改接收器逻辑 | `receiver_dongle/receiver_dongle.ino` | USB桥接+测速转发 |

## Code Map

| Symbol | Type | Location | Role |
|--------|------|----------|------|
| `MotorState` | struct | `motor_control.h` | 单个电机状态 |
| `VehicleMotion` | struct | `motor_control.h` | 整车运动状态 |
| `MotorDirection` | enum class | `motor_control.h` | 方向枚举 |
| `WirelessPacket` | struct | `wireless.h` | 通信数据包（12字节） |
| `OdometryPacket` | struct | `wireless.h` | 测速数据包 |
| `EncoderConfig` | struct | `odometer.h` | 编码器配置 |
| `WheelSpeed` | struct | `odometer.h` | 单轮速度数据 |
| `OdometryData` | struct | `odometer.h` | 整车测速数据 |
| `PIDParams` | struct | `pid_control.h` | PID参数 |
| `PIDState` | struct | `pid_control.h` | PID计算状态 |
| `SmartMotorOutput` | struct | `pid_control.h` | 智能电机输出 |
| `DriveMode` | enum class | `pid_control.h` | 行走模式 |
| `CameraConfiguration` | struct | `camera_config.h` | 摄像头配置 |
| `FrameState` | struct | `video_stream.h` | 视频帧状态 |

## Conventions

- **函数式编程**：所有数据用 `const`，状态通过函数返回值传递
- **中文注释**：所有函数、结构体、枚举都有详细中文注释
- **命名空间**：每个模块独立命名空间（`PinConfig`, `SG90Config`）
- **不可变状态**：`const struct` 存储状态，新状态通过函数创建
- **硬件分离**：纯函数处理逻辑，`apply_*` 函数处理硬件副作用

## Anti-Patterns

- **禁止使用全局可变状态**：所有状态必须显式传递
- **禁止隐式转换**：`enum class` 替代 `enum`
- **禁止混合关注点**：电机、舵机、无线逻辑完全分离
- **禁止空 catch 块**：所有错误必须处理

## Commands

```bash
# Arduino IDE 上传
# 1. 打开 .ino 文件
# 2. 选择工具 -> 开发板 -> ESP32C6 Dev Module（或 ESP32S3 Dev Module）
# 3. 选择端口
# 4. 点击上传
```

## Notes

- **ESP-NOW信道**：固定 channel 1，所有设备必须一致
- **MAC 地址**：在 `libraries/wireless_protocol/src/wireless.h` 中配置，支持运行时动态修改
- **PWM 频率**：电机 1kHz
- **内存**：视频帧缓冲 32768 字节，注意不要溢出
- **电源**：电机和逻辑电源必须隔离，共地
- **HardwareSerial**：ESP32-S3 与 ESP32-C6 通过 GPIO 14/15 的 Serial1 通信，波特率 921600
