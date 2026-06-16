/**
 * 智能车控制器主程序 - ESP32-C6
 * 基于函数式编程思想，使用Arduino IDE开发
 * 
 * 功能：
 * 1. 接收ESP-NOW无线命令（来自接收器）
 * 2. 控制L298N驱动4个电机
 * 3. 测速模块：编码器读取+速度计算
 * 4. PID控制：直线修正+精确方向
 * 5. 发送状态反馈（含测速数据）
 * 6. 接收摄像头视频帧并转发到接收器
 *
 * 硬件接线：
 * - L298N #1: IN1->GPIO4, IN2->GPIO5, EN->GPIO6 (控制左侧电机)
 * - L298N #2: IN1->GPIO7, IN2->GPIO8, EN->GPIO9 (控制右侧电机)
 * - 左编码器: GPIO0
 * - 右编码器: GPIO1
 * - Serial1: GPIO2(RX) / GPIO3(TX) -> ESP32-S3 UART（与摄像头模块通信）
 * 
 * 作者：智能车项目团队
 * 版本：1.3.0
 */

#include "motor_control.h"
#include "../libraries/wireless_protocol/src/wireless.h"
#include "odometer.h"
#include "pid_control.h"
#include <BLEDevice.h>

// Serial1 配置：与 ESP32-S3 摄像头模块通信（硬件串口，GPIO2 RX / GPIO3 TX）
namespace SoftSerialConfig {
    constexpr uint8_t RX_PIN = 2;        // Serial1 接收引脚（GPIO2）
    constexpr uint8_t TX_PIN = 3;        // Serial1 发送引脚（GPIO3）
    constexpr uint32_t BAUD_RATE = 921600; // 波特率（高速传输视频帧）
    constexpr size_t FRAME_BUFFER_SIZE = 32768; // 视频帧缓冲区大小（32KB）
}

// ============================================
// 调试配置（条件编译开关）
// 设为 1 启用对应模块的调试日志，0 关闭
// 生产环境应全部设为 0 以减少串口占用和CPU开销
// ============================================
#define DEBUG_MOTOR 0      // 电机调试日志
#define DEBUG_WIRELESS 0   // 无线调试日志
#define DEBUG_ODOMETRY 0   // 测速调试日志
#define DEBUG_PID 0        // PID调试日志

// ============================================
// 全局状态（可变状态，在主循环中更新）
// ============================================

/// 硬件串口 Serial1（与摄像头模块通信，GPIO2 RX / GPIO3 TX）

/// 视频帧缓冲区（从摄像头接收的帧数据）
uint8_t g_cameraFrameBuffer[SoftSerialConfig::FRAME_BUFFER_SIZE];
size_t g_cameraFrameSize = 0;
bool g_cameraFrameReady = false;

/**
 * 当前车辆运动状态
 * 每次命令更新时创建新状态
 */
VehicleMotion g_currentMotion = createStopState();

/**
 * 当前速度值（0-255）
 */
uint8_t g_currentSpeed = 28;

/**
 * 最后命令接收时间
 */
uint32_t g_lastCmdTime = 0;

/**
 * 紧急停止标志
 */
bool g_emergencyStop = false;

/**
 * 测速数据上报间隔(ms)
 */
constexpr uint16_t ODOMETRY_REPORT_INTERVAL_MS = 200;
uint32_t g_lastOdomReportTime = 0;

/**
 * 直线修正使能标志
 */
bool g_smartDriveEnabled = false;

// ============================================
// 命令处理函数
// ============================================

/**
 * 处理运动命令（带智能修正）
 * 输入：WASD命令字符
 * 效果：更新车辆运动状态（可能带PID修正）
 */
void handleMoveCommand(const char cmd) {
    // 创建基础运动状态
    g_currentMotion = parseWASDCommand(cmd, g_currentSpeed);
    
    // 如果启用智能修正，应用PID修正
    if (g_smartDriveEnabled && cmd != ' ') {
        // 获取当前运动方向
        MotorDirection leftDir = g_currentMotion.frontLeft.direction;
        MotorDirection rightDir = g_currentMotion.frontRight.direction;
        
        // 只有前后运动才做直线修正（转弯不需要）
        if ((leftDir == MotorDirection::FORWARD && rightDir == MotorDirection::FORWARD) ||
            (leftDir == MotorDirection::BACKWARD && rightDir == MotorDirection::BACKWARD)) {
            
            // 更新测速数据
            updateOdometer();
            
            // 应用PID智能修正
            SmartMotorOutput output = updateSmartControl(
                g_currentSpeed, leftDir, rightDir
            );
            
            // 创建修正后的差速运动状态
            g_currentMotion = createDifferentialState(
                output.leftDir, output.leftPwm,
                output.rightDir, output.rightPwm
            );
        }
    }
    
    // 应用状态到硬件
    applyVehicleMotion(g_currentMotion);
    
    // 更新时间戳
    g_lastCmdTime = millis();
    
#if DEBUG_MOTOR
    Serial.printf("[运动命令] 执行: %c, 速度: %d, 智能修正: %s\n", 
                  cmd, g_currentSpeed, 
                  g_smartDriveEnabled ? "ON" : "OFF");
#endif
}

/**
 * 处理速度命令
 * 输入：速度值
 */
void handleSpeedCommand(const uint8_t speed) {
    g_currentSpeed = speed;
    g_lastCmdTime = millis();  // 更新时间戳，防止超时自动停止
#if DEBUG_MOTOR
    Serial.printf("[速度设置] 新速度: %d\n", g_currentSpeed);
#endif
}

/**
 * 处理停止命令
 */
void handleStopCommand() {
    g_currentMotion = createStopState();
    applyVehicleMotion(g_currentMotion);
    g_emergencyStop = true;
#if DEBUG_MOTOR
    Serial.println("[紧急停止] 车辆已停止");
#endif
}

/**
 * 处理校准命令
 * 在车直线行驶一段距离后发送此命令自动校准
 */
void handleCalibrateCommand() {
    SpeedCalibration calib = autoCalibrate();
    setSpeedCalibration(calib.leftCorrection, calib.rightCorrection);
#if DEBUG_PID
    Serial.println("[校准完成] 左右轮修正系数已更新");
#endif
}

/**
 * 处理行走模式切换命令
 * 数据字节值：
 *   0 = 普通模式（无修正）
 *   1 = 直线修正模式
 *   2 = 航向锁定模式
 */
void handleDriveModeCommand(const uint8_t mode) {
    switch (mode) {
        case 0:
            setDriveMode(DriveMode::NORMAL);
            g_smartDriveEnabled = false;
            break;
        case 1:
            setDriveMode(DriveMode::STRAIGHT_LINE);
            g_smartDriveEnabled = true;
            break;
        case 2:
            setDriveMode(DriveMode::HEADING_LOCK);
            g_smartDriveEnabled = true;
            break;
        default:
#if DEBUG_MOTOR
            Serial.printf("[行走模式] 未知模式: %d\n", mode);
#endif
            break;
    }
}

/**
 * 发送测速数据到接收器
 * 通过ESP-NOW发送OdometryPacket
 */
void sendOdometryData() {
    const OdometryData odom = getCurrentOdometry();
    
    // 将浮点数据压缩为整数（有符号16位），使用 constrain 防止溢出
    // 注意：odom.leftWheel.mmps 已在 updateOdometer() 中应用了校准系数，
    //       此处直接使用，避免双重校准
    const int16_t leftSpeed = static_cast<int16_t>(constrain(
        static_cast<long>(odom.leftWheel.mmps),
        INT16_MIN, INT16_MAX
    ));
    const int16_t rightSpeed = static_cast<int16_t>(constrain(
        static_cast<long>(odom.rightWheel.mmps),
        INT16_MIN, INT16_MAX
    ));
    const int16_t headingX100 = static_cast<int16_t>(constrain(
        static_cast<long>(odom.heading * 100.0f),
        INT16_MIN, INT16_MAX
    ));
    const uint16_t totalDist = static_cast<uint16_t>(
        fmin(odom.distanceMm, 65535.0f)
    );
    
    // 创建测速数据包
    OdometryPacket packet(
        WirelessConfig::MAGIC_BYTE,
        WirelessConfig::PROTOCOL_VERSION,
        CommandType::ODOMETRY,
        leftSpeed,
        rightSpeed,
        headingX100,
        totalDist,
        0   // 校验和暂填0
    );
    
    // 计算校验和
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&packet);
    uint8_t checksum = 0;
    for (size_t i = 0; i < sizeof(packet) - 1; i++) {
        checksum += data[i];
    }
    
    // 创建带校验和的包（通过重新构造）
    const OdometryPacket finalPacket(
        packet.magic, packet.version, packet.type,
        packet.leftSpeedMmps, packet.rightSpeedMmps,
        packet.headingX100, packet.totalDistMm, checksum
    );
    
    // 发送到接收器
    // 注意：直接发送 OdometryPacket（12字节），通过通用发送函数避免 reinterpret_cast UB
    sendRawPacket(WirelessConfig::RECEIVER_MAC, 
                  reinterpret_cast<const uint8_t*>(&finalPacket), 
                  sizeof(finalPacket));
}

// ============================================
// 摄像头视频帧接收与转发
// ============================================

/**
 * 从串口1（Serial1）接收摄像头视频帧
 * 帧格式: [0xAA][0x55][帧大小4字节][帧数据]
 * 接收完整帧后标记为待转发
 */
/// 视频帧接收状态机
static enum { WAIT_HEADER1, WAIT_HEADER2, READ_SIZE, READ_DATA } g_frameState = WAIT_HEADER1;
static uint8_t g_sizeBytes[4];
static size_t g_sizeBytesRead = 0;
static uint32_t g_expectedSize = 0;

void receiveCameraFrame() {
    // 如果当前帧已就绪但尚未转发，暂停接收新数据
    // 防止 forwardCameraFrame() 发送期间缓冲区被覆盖
    if (g_cameraFrameReady) return;
    
    while (Serial1.available()) {
        const int byteVal = Serial1.read();
        if (byteVal < 0) return;
        
        switch (g_frameState) {
            case WAIT_HEADER1:
                if (byteVal == 0xAA) g_frameState = WAIT_HEADER2;
                break;
            case WAIT_HEADER2:
                if (byteVal == 0x55) {
                    g_frameState = READ_SIZE;
                    g_sizeBytesRead = 0;
                } else if (byteVal == 0xAA) {
                    // 连续 0xAA，继续等待 0x55
                } else {
                    g_frameState = WAIT_HEADER1;
                }
                break;
            case READ_SIZE:
                g_sizeBytes[g_sizeBytesRead++] = static_cast<uint8_t>(byteVal);
                if (g_sizeBytesRead >= 4) {
                    g_expectedSize = (static_cast<uint32_t>(g_sizeBytes[0])) |
                                     (static_cast<uint32_t>(g_sizeBytes[1]) << 8) |
                                     (static_cast<uint32_t>(g_sizeBytes[2]) << 16) |
                                     (static_cast<uint32_t>(g_sizeBytes[3]) << 24);
                    if (g_expectedSize == 0 || g_expectedSize > SoftSerialConfig::FRAME_BUFFER_SIZE) {
                        // 帧大小异常，重置状态
                        g_frameState = WAIT_HEADER1;
                    } else {
                        g_cameraFrameSize = 0;
                        g_frameState = READ_DATA;
                    }
                }
                break;
            case READ_DATA:
                if (g_cameraFrameSize < g_expectedSize) {
                    g_cameraFrameBuffer[g_cameraFrameSize++] = static_cast<uint8_t>(byteVal);
                    if (g_cameraFrameSize >= g_expectedSize) {
                        // 完整帧接收完成
                        g_cameraFrameReady = true;
                        g_frameState = WAIT_HEADER1;
                    }
                }
                break;
        }
    }
}

/**
 * 将摄像头视频帧通过 ESP-NOW 转发到接收器
 * 使用 VideoPacket 分包传输（与原 video_stream.h 相同的协议）
 */
void forwardCameraFrame() {
    if (!g_cameraFrameReady) return;
    g_cameraFrameReady = false;
    
    const uint8_t* data = g_cameraFrameBuffer;
    const size_t totalLen = g_cameraFrameSize;
    const uint16_t totalPackets = (totalLen + StreamConfig::MAX_PACKET_SIZE - 1) / 
                                   StreamConfig::MAX_PACKET_SIZE;
    
    static uint16_t forwardFrameId = 0;
    forwardFrameId++;
    
    for (uint16_t i = 0; i < totalPackets; i++) {
        const size_t offset = i * StreamConfig::MAX_PACKET_SIZE;
        const uint16_t packetLen = min(
            static_cast<size_t>(StreamConfig::MAX_PACKET_SIZE),
            totalLen - offset
        );
        
        // 构建视频包
        VideoPacket packet = {};
        packet.magic = StreamConfig::VIDEO_MAGIC;
        packet.version = StreamConfig::PROTOCOL_VERSION;
        packet.frameId = forwardFrameId;
        packet.packetId = i;
        packet.totalPackets = totalPackets;
        packet.dataLen = packetLen;
        memcpy(packet.data, data + offset, packetLen);
        
        // 计算实际发送大小：10字节头部 + packetLen字节数据 + 1字节校验和
        const size_t sendSize = 10 + packetLen + 1;
        
        // 计算校验和：覆盖发送范围内除校验和字节外的所有字节
        uint8_t sum = 0;
        const uint8_t* packetData = reinterpret_cast<const uint8_t*>(&packet);
        for (size_t j = 0; j < sendSize - 1; j++) {
            sum += packetData[j];
        }
        
        // 校验和写入实际发送的最后一个字节位置
        packet.data[packetLen] = sum;
        
        // 通过 ESP-NOW 发送到接收器
        sendRawPacket(WirelessConfig::RECEIVER_MAC,
                     reinterpret_cast<const uint8_t*>(&packet),
                     sendSize);
        
        // 短暂延迟避免拥塞
        delayMicroseconds(50);
    }
}

// ============================================
// ESP-NOW 接收回调
// ============================================

void onDataRecv(const esp_now_recv_info* info, const uint8_t* incomingData, int len) {
    // 非标准长度包日志（调试用，生产环境通过 DEBUG_WIRELESS 开关控制）
    if (len != sizeof(WirelessPacket)) {
#if DEBUG_WIRELESS
        Serial.printf("[无线通信] 收到非标准长度包: %d 字节（期望 %d）\n",
                      len, static_cast<int>(sizeof(WirelessPacket)));
#endif
        return;
    }

    const WirelessPacket* packet = reinterpret_cast<const WirelessPacket*>(incomingData);
    
    if (!validatePacket(*packet)) {
#if DEBUG_WIRELESS
        Serial.println("[无线通信] 收到无效数据包");
#endif
        return;
    }
    
    // 处理命令
    switch (packet->type) {
        case CommandType::MOVE:
            g_emergencyStop = false;  // 运动命令显式解除紧急停止
            handleMoveCommand(static_cast<char>(packet->data));
            break;
        case CommandType::SPEED:
            handleSpeedCommand(packet->speed);
            break;
        case CommandType::STOP:
            handleStopCommand();
            break;
        case CommandType::STATUS:
            // 心跳命令只更新时间戳，防止超时自动停止
            // 测速数据已由 loop() 中的 200ms 定时器独立发送，无需重复发送
            g_lastCmdTime = millis();
            break;
        case CommandType::CALIBRATE:
            handleCalibrateCommand();
            break;
        case CommandType::DRIVE_MODE:
            handleDriveModeCommand(packet->data);
            g_lastCmdTime = millis();
            break;
        default:
            break;
    }
}

// ============================================
// 初始化
// ============================================

void setup() {
    // 初始化串口
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n================================");
    Serial.println("智能车控制系统 - ESP32-C6");
    Serial.println("版本: 1.3.0 (含测速+PID+视频转发)");
    Serial.println("================================\n");
    
    // 初始化串口1（与摄像头模块通信，硬件串口自带硬件缓冲，可承受921600波特率）
    Serial1.begin(SoftSerialConfig::BAUD_RATE, SERIAL_8N1, SoftSerialConfig::RX_PIN, SoftSerialConfig::TX_PIN);
    delay(100);
    Serial.println("[初始化] 串口1初始化完成 (GPIO2 RX, GPIO3 TX)");
    
    // 初始化电机引脚
    initializeMotorPins();
    delay(100);
    
    // 初始化测速模块
    initializeOdometer();
    delay(100);
    
    // 初始化PID控制器
    initializePIDController();
    delay(100);
    
    // 初始化无线通信
    if (!initializeWireless(DeviceRole::CAR)) {
        Serial.println("[初始化] 无线通信初始化失败，重启中...");
        delay(1000);
        ESP.restart();
    }
    
    // 注册接收回调
    if (esp_now_register_recv_cb(onDataRecv) != ESP_OK) {
        Serial.println("[无线通信] 注册接收回调失败");
    }
    
    Serial.print("[初始化] MAC: ");
    Serial.println(WiFi.macAddress());
    
    // 初始化 BLE 设备名（方便扫描识别）
    BLEDevice::init("智能车");
    
    // 初始化状态
    g_currentMotion = createStopState();
    g_currentSpeed = 28;
    g_emergencyStop = false;
    // g_smartDriveEnabled 保持全局声明时的初始值 false，匹配前端默认 OFF
    
    Serial.println("[初始化] 系统启动完成，等待命令...");
    Serial.println("[命令说明]");
    Serial.println("  WASD: 移动控制");
    Serial.println("  Q/E: 原地旋转");
    Serial.println("  空格: 停止");
    Serial.println("  1-9: 速度设置");
    Serial.println("  智能修正: 默认关闭");
}

// ============================================
// 主循环
// ============================================

void loop() {
    const uint32_t currentTime = millis();
    
    // 1. 从摄像头接收视频帧
    receiveCameraFrame();
    
    // 2. 转发视频帧到接收器
    forwardCameraFrame();
    
    // 3. 定期更新测速数据并发送（与采样周期对齐，避免无效调用）
    if (currentTime - g_lastOdomReportTime >= ODOMETRY_REPORT_INTERVAL_MS) {
        updateOdometer();
        sendOdometryData();
        g_lastOdomReportTime = currentTime;
    }
    
    // 4. 检查通信超时
    if (!g_emergencyStop && (currentTime - g_lastCmdTime) > 1000) {
        // 超过1秒未收到命令，自动停止
        if (g_currentMotion.frontLeft.direction != MotorDirection::STOP) {
            g_currentMotion = createStopState();
            applyVehicleMotion(g_currentMotion);
#if DEBUG_MOTOR
            Serial.println("[超时] 自动停止");
#endif
        }
    }

    // 5. 小延迟，避免占用过多CPU
    delay(1);
}