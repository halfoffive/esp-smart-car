# 嵌入式固件 - Knowledge Base

**Location:** `firmware/`
**Language:** Arduino/C++ (ESP32)
**IDE:** Arduino IDE

## Structure

```
firmware/
├── libraries/
│   └── wireless_protocol/   # Arduino 库：WiFi/UDP 通信协议（应用层包格式）
│       └── src/
│           └── wireless.h   # 共享头文件（WirelessPacket、OdometryPacket、VideoPacket）
├── car_controller/          # 车载 ESP32-S3（Freenove FNK0085 单芯片架构）
│   ├── car_controller.ino   # 主程序（摄像头采集 + 电机控制 + WiFi STA + UDP，无 BLE 广播）
│   ├── motor_control.h      # 函数式电机控制（L298N 驱动 2 个电机，左/右各一路）
│   ├── odometer.h           # 编码器测速模块（GPIO 1/2 中断）
│   ├── pid_control.h        # PID控制器（直线修正 + 航向锁定）
│   ├── camera_config.h      # OV2640 摄像头配置（ESP32-S3 CAM 标准引脚）
│   └── video_stream.h       # 视频流传输（WiFi UDP 分包直发接收器）
└── receiver_dongle/         # 接收器 ESP32-C6（USB dongle）
    └── receiver_dongle.ino  # USB 二进制 WirelessPacket 桥接 + 测速转发 + BLE 扫描 + 链路状态上报
```

## Where to Look

| Task | Location | Notes |
|------|----------|-------|
| 修改电机控制 | `car_controller/motor_control.h` | 纯函数，差速控制，双电机模型（left/right） |
| 修改无线协议 | `libraries/wireless_protocol/src/wireless.h` | 应用层包格式（WirelessPacket/OdometryPacket/VideoPacket），C6 AP / S3 STA / UDP 端口 9000/9001 |
| 修改测速模块 | `car_controller/odometer.h` | 编码器中断+速度计算 |
| 修改PID控制 | `car_controller/pid_control.h` | 直线修正+航向锁定 |
| 修改摄像头配置 | `car_controller/camera_config.h` | 分辨率/质量/引脚 |
| 修改视频传输 | `car_controller/video_stream.h` | WiFi UDP 分包传输（128字节/包）到 C6 遥测端口 9001 |
| 修改接收器逻辑 | `receiver_dongle/receiver_dongle.ino` | USB 二进制 WirelessPacket 桥接 + UDP 控制转发 + 遥测/视频转发 + BLE扫描（仅扫描周边设备） |
| 修改车载主循环 | `car_controller/car_controller.ino` | 视频采集 + 电机控制 + WiFi STA + UDP 收发 |

## Code Map

| Symbol | Type | Location | Role |
|--------|------|----------|------|
| `MotorState` | struct | `motor_control.h` | 单个电机状态 |
| `VehicleMotion` | struct | `motor_control.h` | 整车运动状态（left/right 双电机） |
| `MotorDirection` | enum class | `motor_control.h` | 方向枚举 |
| `WirelessPacket` | struct | `wireless.h` | 通信数据包（12字节），UDP/串口 应用层 payload；`speed` 字段直接表示 0-255 PWM |
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

- **WiFi 链路**：C6 作为固定 AP（SSID "ESP-SmartCar" / 密码 "SmartCar2024" / IP 192.168.4.1 / 最大功率），S3 作为 STA 使用静态 IP 192.168.4.2
- **UDP 端口**：控制命令走 9000（C6→S3），遥测/视频走 9001（S3→C6），应用层继续使用 WirelessPacket/OdometryPacket/VideoPacket
- **PWM 频率**：电机 1kHz（`analogWrite` 默认）
- **内存**：接收器视频帧缓冲 32768 字节（`ReceiverConfig::BUFFER_SIZE`），注意不要溢出
- **电源**：电机和逻辑电源必须隔离，共地
- **单芯片架构**：车载 ESP32-S3 同时承担摄像头采集 + 电机控制 + 编码器测速 + PID + WiFi STA UDP 收发；S3 不再进行 BLE 广播
- **速度控制**：`WirelessPacket.speed` 直接表示电机 PWM 占空比（0-255），不再经过 1-9 档位映射
- **视频传输**：ESP32-S3 通过 WiFi UDP 直发视频帧到接收器（无 Serial1 桥接），分包大小 128 字节，端口 9001
- **PC → 接收器串口协议**：12 字节二进制 `WirelessPacket`；`CommandType::BLE_SCAN = 10` 和 `CommandType::LINK_STATUS = 11` 由接收器本地处理，不转发到 S3
- **BLE 扫描**：接收器处理 `CommandType::BLE_SCAN = 10` 进行通用周边设备扫描；S3 已不再广播 WiFi MAC，固定热点/密码取代 MAC 配对
- **链路状态**：接收器处理 `CommandType::LINK_STATUS = 11` 并在请求时或每 5 秒主动上报 `{"t":"link",...}` JSON，后端解析后推送前端
- **测速上报**：车载端每 200ms 通过 WiFi UDP 端口 9001 发送 `OdometryPacket`，接收器转为 JSON 转发到 PC
- **编码器**：光电编码器（光栅码盘 + 光敏元件），GPIO 1/2，20 脉冲/圈，下降沿（FALLING）触发中断；ISR 函数定义在 `car_controller.ino` 中（非 `inline`），避免 `IRAM_ATTR` 重定位错误
