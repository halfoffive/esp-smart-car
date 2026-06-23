/**
 * 智能车控制器主程序 - ESP32-S3（Freenove FNK0085 单芯片架构）
 * 基于函数式编程思想，使用Arduino IDE开发
 * 
 * 功能：
 * 1. 通过 WiFi STA + UDP 接收无线命令（来自接收器/AP）
 * 2. 控制L298N驱动2个电机（左/右双电机）
 * 3. 测速模块：编码器读取+速度计算
 * 4. PID控制：直线修正+精确方向
 * 5. 发送状态反馈（含测速数据）
 * 6. 摄像头采集视频帧并通过 WiFi/UDP 直发到接收器（S3 单芯片，无 Serial1 桥接）
 *
 * 硬件接线（ESP32-S3 WROOM CAM，Freenove FNK0085）：
 * - 摄像头（OV2640）: GPIO 4-18（除 GPIO 14 外，均为摄像头专用引脚）
 * - L298N #1: IN1->GPIO38, IN2->GPIO39, EN->GPIO40 (控制左侧电机)
 * - L298N #2: IN1->GPIO41, IN2->GPIO42, EN->GPIO21 (控制右侧电机)
 * - 左编码器: GPIO1（中断引脚）
 * - 右编码器: GPIO2（中断引脚）
 * 
 * 作者：智能车项目团队
 * 版本：2.0.0（整帧单包传输 + 多任务架构：videoTask Core0 + loop Core1）
 * 日期：2026-06-20
 */

#include "motor_control.h"
#include "../libraries/wireless_protocol/src/wireless.h"
#include "odometer.h"
#include "pid_control.h"
#include "camera_config.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include "video_stream.h"

// 版本常量（统一 car_controller / video_stream / camera_config 的对外版本号）
constexpr const char* VERSION = "1.10.0";

// UDP 套接字（video_stream.h 中通过 extern 声明，在同一 sketch 中定义即可）
WiFiUDP g_udpControl;
WiFiUDP g_udpTelemetry;

// ============================================
// 调试配置（条件编译开关）
// 设为 1 启用对应模块的调试日志，0 关闭
// 生产环境应全部设为 0 以减少串口占用和CPU开销
// 当前处于开发环境，全部设置为1,如果你是AI,没有用户允许，不要更改日志级别。
// ============================================
#define DEBUG_MOTOR 1     // 电机调试日志
#define DEBUG_WIRELESS 1  // 无线调试日志
#define DEBUG_ODOMETRY 1  // 测速调试日志
#define DEBUG_PID 1       // PID调试日志

// ============================================
// 全局状态（可变状态，在主循环中更新）
// ============================================

/**
 * 当前车辆运动状态
 * 每次命令更新时创建新状态
 */
VehicleMotion g_currentMotion = VehicleMotion(
    MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
               MotorDirection::STOP, 0),
    MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
               MotorDirection::STOP, 0)
);

/**
 * 当前速度值（PWM 0-255）
 * 初始值 128 对应中速，避免首次连接未发送速度命令前车速与前端显示不一致
 */
uint8_t g_currentSpeed = 128;

/**
 * 最后接受的控制包序列号（用于反重放，u16 回绕窗口）
 */
uint16_t g_lastAcceptedSeq = 0;

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
 * 与 OdometerConfig::SAMPLE_PERIOD_MS 保持一致，避免无效调用
 */
constexpr uint16_t ODOMETRY_REPORT_INTERVAL_MS = OdometerConfig::SAMPLE_PERIOD_MS;

/**
 * 命令超时自动停止时间(ms)
 * 超过此时间未收到任何有效命令（运动/速度/心跳/行走模式）则自动停车
 */
constexpr uint32_t COMMAND_TIMEOUT_MS = 1000;
// 初始值 0 标记"未初始化"，setup() 末尾赋值为 millis()，避免首轮立即触发测速发送
uint32_t g_lastOdomReportTime = 0;

/**
 * 直线修正使能标志
 */
bool g_smartDriveEnabled = false;

/**
 * 摄像头配置（运行时复用，错误恢复时重新初始化）
 */
CameraConfiguration g_cameraConfig = createDefaultConfig();

/**
 * 最后生效的运动命令字符（SPEED 变化时重发，保持当前运动状态）
 */
char g_lastMoveCmd = ' ';

// ============================================
// 编码器中断服务函数（定义在此处而非 odometer.h，避免 inline + IRAM_ATTR 导致的 literal pool 重定位错误）
// ============================================

// 全局状态定义（头文件中已声明为 extern，此处做唯一定义）
namespace OdometerState {
    volatile uint32_t g_leftPulses = 0;
    volatile uint32_t g_rightPulses = 0;
    uint32_t g_lastLeftPulses = 0;
    uint32_t g_lastRightPulses = 0;
    float g_leftDistanceMm = 0.0f;
    float g_rightDistanceMm = 0.0f;
    float g_leftSpeedMmps = 0.0f;
    float g_rightSpeedMmps = 0.0f;
    float g_leftRpm = 0.0f;
    float g_rightRpm = 0.0f;
    float g_heading = 0.0f;
    float g_totalDistanceMm = 0.0f;
    uint32_t g_lastSampleTime = 0;
    SpeedCalibration g_calibration = OdometerConfig::DEFAULT_CALIBRATION;
}

namespace PIDControllerState {
    PIDState g_straightPidState = PIDState(0, 0, 0, 0, 0, 0, 0);
    PIDState g_headingPidState = PIDState(0, 0, 0, 0, 0, 0, 0);
    DriveMode g_driveMode = DriveMode::NORMAL;
    float g_headingLockTarget = 0.0f;
    bool g_headingLockTargetInitialized = false;
}

void IRAM_ATTR onLeftEncoderPulse() {
    __atomic_fetch_add(&OdometerState::g_leftPulses, 1, __ATOMIC_RELAXED);
}

void IRAM_ATTR onRightEncoderPulse() {
    __atomic_fetch_add(&OdometerState::g_rightPulses, 1, __ATOMIC_RELAXED);
}

// ============================================
// 命令处理函数
// ============================================

/**
 * 处理运动命令（带智能修正）
 * 输入：WASD命令字符
 * 效果：更新车辆运动状态（可能带PID修正）
 */
void handleMoveCommand(const char cmd) {
  // 解析命令字符到运动状态；未知命令视为停止
  if (!commandToVehicleMotion(cmd, g_currentSpeed, g_currentMotion)) {
    g_currentMotion = VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::STOP, 0),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::STOP, 0)
    );
    applyVehicleMotion(g_currentMotion);
    g_lastCmdTime = millis();
#if DEBUG_MOTOR
    Serial.printf("[运动命令] 未知命令: %c，车辆停止\n", cmd);
#endif
    return;
  }

  // 缓存有效运动命令（空格停止命令不覆盖之前的移动缓存）
  if (cmd != ' ') {
    g_lastMoveCmd = cmd;
  }

  // 如果启用智能修正，应用PID修正
  if (g_smartDriveEnabled && cmd != ' ') {
    // 获取当前运动方向
    MotorDirection leftDir = g_currentMotion.left.direction;
    MotorDirection rightDir = g_currentMotion.right.direction;

    // 只有前后运动才做直线修正（转弯不需要）
    if ((leftDir == MotorDirection::FORWARD && rightDir == MotorDirection::FORWARD) || (leftDir == MotorDirection::BACKWARD && rightDir == MotorDirection::BACKWARD)) {

      // 更新测速数据（传入当前电机方向，使后退时里程/航向符号正确）
      updateOdometer(leftDir, rightDir);

      // 应用PID智能修正
      SmartMotorOutput output = updateSmartControl(
        g_currentSpeed, leftDir, rightDir);

      // 创建修正后的差速运动状态
      g_currentMotion = VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   output.leftDir, output.leftPwm),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   output.rightDir, output.rightPwm)
      );
    }
  }

  // 应用状态到硬件
  applyVehicleMotion(g_currentMotion);

  // 更新时间戳
  g_lastCmdTime = millis();

#if DEBUG_MOTOR
  Serial.printf("[运动命令] 执行: %c, 速度(PWM 0-255): %d, 智能修正: %s\n",
                cmd, g_currentSpeed,
                g_smartDriveEnabled ? "ON" : "OFF");
#endif
}

/**
 * 处理速度命令
 * 输入：速度值（PWM 0-255）
 */
void handleSpeedCommand(const uint8_t speed) {
  g_currentSpeed = speed;
  g_lastCmdTime = millis();  // 更新时间戳，防止超时自动停止

  // 速度变化时立即用缓存的运动命令重发，保持当前运动状态同步
  if (!g_emergencyStop) {
    handleMoveCommand(g_lastMoveCmd);
  }

#if DEBUG_MOTOR
  Serial.printf("[速度设置] 新速度(PWM 0-255): %d\n", g_currentSpeed);
#endif
}

/**
 * 处理停止命令
 */
void handleStopCommand() {
    g_currentMotion = VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::STOP, 0),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::STOP, 0)
    );
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
  SpeedCalibration calib = autoCalibrate(
    g_currentMotion.left.direction, g_currentMotion.right.direction);
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
 * 通过 WiFi UDP 发送 OdometryPacket
 */
void sendOdometryData() {
  // WiFi 守卫：未连接时跳过发送，避免 lwIP socket 野指针 → StoreProhibited
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  const OdometryData odom = getCurrentOdometry();

  // 将浮点数据压缩为整数（有符号16位），使用 constrain 防止溢出
  // 注意：odom.leftWheel.mmps 已在 updateOdometer() 中应用了校准系数，
  //       此处直接使用，避免双重校准
  const int16_t leftSpeed = static_cast<int16_t>(constrain(
    static_cast<long>(odom.leftWheel.mmps),
    INT16_MIN, INT16_MAX));
  const int16_t rightSpeed = static_cast<int16_t>(constrain(
    static_cast<long>(odom.rightWheel.mmps),
    INT16_MIN, INT16_MAX));
  const int16_t headingX100 = static_cast<int16_t>(constrain(
    static_cast<long>(odom.heading * 100.0f),
    INT16_MIN, INT16_MAX));
  const uint16_t totalDist = static_cast<uint16_t>(
    fmin(odom.distanceMm, 65535.0f));

  // 创建测速数据包（aggregate initialization，WirelessPacket/OdometryPacket 已删除构造函数）
  OdometryPacket packet{};
  packet.magic = WirelessConfig::MAGIC_BYTE;
  packet.version = WirelessConfig::PROTOCOL_VERSION;
  packet.type = CommandType::ODOMETRY;
  packet.leftSpeedMmps = leftSpeed;
  packet.rightSpeedMmps = rightSpeed;
  packet.headingX100 = headingX100;
  packet.totalDistMm = totalDist;
  packet.checksum = 0;  // 校验和暂填0

  // 计算校验和
  const uint8_t* data = reinterpret_cast<const uint8_t*>(&packet);
  uint8_t checksum = 0;
  for (size_t i = 0; i < sizeof(packet) - 1; i++) {
    checksum += data[i];
  }
  packet.checksum = checksum;

  // 通过 UDP 发送测速数据到接收器（AP），使用遥测端口
  IPAddress apIp(NetworkConfig::AP_IP[0], NetworkConfig::AP_IP[1], NetworkConfig::AP_IP[2], NetworkConfig::AP_IP[3]);
  g_udpTelemetry.beginPacket(apIp, UdpConfig::TELEMETRY_PORT);
  g_udpTelemetry.write(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
  if (!g_udpTelemetry.endPacket()) {
    Serial.println("[UDP] 测速包发送失败");
  }
}

// ============================================
// 摄像头视频帧采集与 WiFi UDP 直发
// ============================================

/**
 * 视频采集与发送任务（FreeRTOS 独立任务）
 * 在 Core 0 上运行，独立处理视频帧采集和 WiFi UDP 发送
 * 与主循环（Core 1）分离，避免视频传输阻塞控制命令处理
 */
void videoTask(void* parameter) {
  (void)parameter;  // 未使用参数

  // 任务初始化日志
  Serial.println("[视频任务] 已启动（Core 0）");

  while (true) {
    // WiFi 守卫：未连接时跳过视频发送，避免底层 lwIP socket 野指针 → StoreProhibited
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // 帧率控制：使用 wireless.h 中的统一常量
    const uint32_t currentTime = millis();
    if (currentTime - g_streamState.lastFrameTime < StreamConfig::FRAME_INTERVAL) {
      vTaskDelay(pdMS_TO_TICKS(1));  // 短暂等待，避免占用 CPU
      continue;
    }

    // 捕获帧
    const FrameState frame = captureFrame();
    if (!frame.isValid) {
      // 帧捕获失败，更新丢弃计数
      g_streamState.droppedFrames++;
      // 连续失败恢复逻辑：超过阈值时先停车再重启摄像头硬件
      g_consecutiveFailures++;
      if (g_consecutiveFailures >= CAMERA_RESTART_THRESHOLD) {
        Serial.printf("[视频流] 连续 %d 次帧捕获失败，重启摄像头...\n", g_consecutiveFailures);
        esp_camera_deinit();
        delay(500);
        if (!initializeCamera(g_cameraConfig)) {
          Serial.println("[视频流] 摄像头重启失败，继续重试...");
        } else {
          Serial.println("[视频流] 摄像头重启成功");
        }
        g_consecutiveFailures = 0;
      }
      continue;
    }

    // 帧捕获成功，重置连续失败计数
    g_consecutiveFailures = 0;

    // 通过 WiFi UDP 整帧发送到接收器（S3 单芯片直发，无 Serial1 桥接）
    const bool sent = sendVideoFrame(frame);

    // 更新流状态：发送失败计为丢帧
    const uint16_t fps = calculateFPS(g_streamState.lastFrameTime, currentTime);
    g_streamState.lastFrameTime = currentTime;
    g_streamState.fps = fps;
    g_streamState.totalFrames++;
    if (sent) {
      g_streamState.bytesSent += static_cast<uint32_t>(frame.frameSize);
    } else {
      g_streamState.droppedFrames++;
    }

    // 释放帧缓冲（先归还 DMA 缓冲，再访问 sensor，避免持帧期间 I2C 竞争导致 StoreProhibited）
    const size_t cachedFrameSize = frame.frameSize;  // frameSize 是栈值，释放帧后仍可读
    releaseFrame(frame);

    // 动态调整质量（渐进阻尼，每步 ±3，稳定在质量 12-35 区间）
    g_currentQuality = adjustQuality(cachedFrameSize, g_currentQuality);
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
      sensor->set_quality(sensor, g_currentQuality);
    }

    // 每100帧打印一次统计
    static uint32_t lastLoggedFrame = 0;
    if (g_streamState.totalFrames != lastLoggedFrame && g_streamState.totalFrames % 100 == 0 && g_streamState.totalFrames > 0) {
      lastLoggedFrame = g_streamState.totalFrames;
      Serial.printf("[视频流] FPS:%d, 总帧:%lu, 丢弃:%lu, 发送:%lu KB\n",
                    g_streamState.fps,
                    (unsigned long)g_streamState.totalFrames,
                    (unsigned long)g_streamState.droppedFrames,
                    (unsigned long)(g_streamState.bytesSent / 1024));
    }
  }
}

// ============================================
// UDP 控制命令接收处理
// ============================================

void handleUdpControlPacket() {
  int len = g_udpControl.parsePacket();
  if (len != sizeof(WirelessPacket) && len > 0) {
    Serial.printf("[UDP] 收到非标准控制包: %d\n", len);
    g_udpControl.flush();
    return;
  }
  if (len == sizeof(WirelessPacket)) {
    // 源地址白名单：控制包必须来自接收器/AP（固定 IP 192.168.4.1）
    const IPAddress remoteIp = g_udpControl.remoteIP();
    const IPAddress apIp(NetworkConfig::AP_IP[0], NetworkConfig::AP_IP[1],
                         NetworkConfig::AP_IP[2], NetworkConfig::AP_IP[3]);
    if (remoteIp != apIp) {
      Serial.printf("[UDP] 控制包来源非法: %s，丢弃\n", remoteIp.toString().c_str());
      return;
    }

    WirelessPacket packet;
    if (g_udpControl.read((uint8_t*)&packet, sizeof(packet)) != sizeof(packet)) {
      Serial.println("[UDP] 控制包读取不完整");
      return;
    }

    if (!validatePacket(packet)) {
      Serial.println("[UDP] 收到无效控制包");
      return;
    }

    // 反重放检查：拒绝旧 seq 或重复 seq（考虑 u16 回绕窗口）
    if (static_cast<int16_t>(packet.seq - g_lastAcceptedSeq) <= 0) {
      Serial.printf("[UDP] 收到旧/重复控制包 seq=%u，丢弃\n", packet.seq);
      return;
    }
    g_lastAcceptedSeq = packet.seq;

    // 处理命令
    switch (packet.type) {
      case CommandType::MOVE:
        g_emergencyStop = false;  // 运动命令显式解除紧急停止
        handleMoveCommand(static_cast<char>(packet.data));
        break;
      case CommandType::SPEED:
        handleSpeedCommand(packet.speed);
        break;
      case CommandType::STOP:
        handleStopCommand();
        break;
      case CommandType::STATUS:
        // 心跳命令只更新时间戳，防止超时自动停止
        // 测速数据已由 loop() 中的 100ms 定时器独立发送，无需重复发送
        g_lastCmdTime = millis();
        break;
      case CommandType::CALIBRATE:
        handleCalibrateCommand();
        break;
      case CommandType::DRIVE_MODE:
        handleDriveModeCommand(packet.data);
        g_lastCmdTime = millis();
        break;
      default:
        break;
    }
  }
}

void checkWiFiConnection() {
  // 仅监测 WiFi 状态变化，不主动重连。
  // ESP32 WiFi 栈内部自动处理重连，手动 reconnect() 反而强制重置导致断连循环。
  static bool wasConnected = false;
  static uint32_t connectedSince = 0;
  const bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (isConnected && !wasConnected) {
    wasConnected = true;
    connectedSince = millis();
    Serial.printf("[WiFi_STA] 已连接，IP: %s\n", WiFi.localIP().toString().c_str());
  } else if (!isConnected && wasConnected) {
    wasConnected = false;
    Serial.println("[WiFi_STA] 连接断开（等待自动重连...）");
  }

  // 首次连接后等待 200ms，让 ARP/lwIP 路由表就绪再发 UDP，避免首帧 endPacket 失败
  if (wasConnected && (millis() - connectedSince < 200)) {
    delay(10);
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
  Serial.println("智能车控制系统 - ESP32-S3（Freenove FNK0085）");
  Serial.printf("版本: %s (S3 单芯片：摄像头+电机+编码器+PID+WiFi STA+UDP，速度 0-255 PWM)\n", VERSION);
  Serial.println("================================\n");

  // PSRAM 诊断（摄像头 DMA 缓冲依赖 PSRAM）
  if (psramFound()) {
    Serial.printf("[内存] PSRAM 已启用: %.1f MB\n", ESP.getPsramSize() / 1048576.0f);
  } else {
    Serial.println("[内存] ⚠ PSRAM 未启用！请在 Arduino IDE 中设置 Tools→PSRAM→OPI PSRAM");
    Serial.println("[内存] 摄像头需要 PSRAM，初始化将失败");
  }
  Serial.println("");

  // 初始化电机引脚
  initializeMotorPins();
  delay(100);

  // 初始化测速模块
  initializeOdometer();
  delay(100);

  // 初始化PID控制器
  initializePIDController();
  delay(100);

  // 初始化 WiFi STA（非阻塞，连接状态由 loop() 轮询）
  WiFi.mode(WIFI_STA);
  IPAddress carIp(NetworkConfig::CAR_IP[0], NetworkConfig::CAR_IP[1], NetworkConfig::CAR_IP[2], NetworkConfig::CAR_IP[3]);
  IPAddress gateway(NetworkConfig::GATEWAY[0], NetworkConfig::GATEWAY[1], NetworkConfig::GATEWAY[2], NetworkConfig::GATEWAY[3]);
  IPAddress subnet(NetworkConfig::SUBNET[0], NetworkConfig::SUBNET[1], NetworkConfig::SUBNET[2], NetworkConfig::SUBNET[3]);
  WiFi.config(carIp, gateway, subnet);
  Serial.printf("[WiFi_STA] 连接热点: %s\n", NetworkConfig::AP_SSID);
  WiFi.begin(NetworkConfig::AP_SSID, NetworkConfig::AP_PASSWORD);
  WiFi.setSleep(false);  // 禁用 WiFi 省电模式，防止空闲断开（视频流需要持续连接）
  Serial.println("[WiFi_STA] 正在连接热点（非阻塞，loop 中轮询）...");
  g_udpControl.begin(UdpConfig::CONTROL_PORT);
  g_udpTelemetry.begin(UdpConfig::TELEMETRY_PORT);

  // 初始化摄像头（S3 单芯片架构：摄像头与电机/编码器/PID 共用同一 MCU）
  if (!initializeCamera(g_cameraConfig)) {
    Serial.println("[摄像头] 初始化失败，系统挂起（请检查 PSRAM/排线）...");
    while (true) {
      delay(1000);
    }
  }

  // 启动视频流（标记流状态为活跃）
  startStreaming();
  Serial.println("[初始化] 视频流状态已就绪，等待 FreeRTOS 任务启动...");

  // 创建视频采集与发送任务（Core 0）
  // 任务栈大小：8192 字节（视频缓冲区较大）
  // 优先级：1（低于主循环的默认优先级，确保控制命令优先）
  BaseType_t result = xTaskCreatePinnedToCore(
    videoTask,           // 任务函数
    "VideoTask",         // 任务名称
    8192,                // 栈大小（字节）
    NULL,                // 任务参数
    1,                   // 优先级（1 = 低优先级，主循环为默认）
    NULL,                // 任务句柄（不需要）
    0                    // 绑定的核心（0 = 视频任务在 Core 0）
  );

  if (result != pdPASS) {
    Serial.println("[错误] 视频任务创建失败，系统挂起");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("[初始化] 视频任务已创建（Core 0，栈8192字节，优先级1）");

  // 初始化状态
  g_currentMotion = VehicleMotion(
      MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                 MotorDirection::STOP, 0),
      MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                 MotorDirection::STOP, 0)
  );
  // 默认中速（PWM 128），与前端默认值一致
  g_currentSpeed = 128;
  g_emergencyStop = false;
  g_lastOdomReportTime = millis();  // 初始化测速时间戳，首轮 100ms 后再发（与 WiFi 稳定期错开）
  // g_smartDriveEnabled 保持全局声明时的初始值 false，匹配前端默认 OFF

  Serial.println("[初始化] 系统启动完成，等待命令...");
  Serial.println("[命令说明]");
  Serial.println("  WASD: 移动控制");
  Serial.println("  Q/E: 原地旋转");
  Serial.println("  空格: 停止");
  Serial.println("  速度: 0-255 PWM");
  Serial.println("  智能修正: 默认关闭");
}

// ============================================
// 主循环
// ============================================

void loop() {
  const uint32_t currentTime = millis();

  // 0. 检查 WiFi 连接并处理 UDP 控制命令
  checkWiFiConnection();
  handleUdpControlPacket();

  // 1. 定期更新测速数据并发送（与采样周期对齐，避免无效调用）
  if (currentTime - g_lastOdomReportTime >= ODOMETRY_REPORT_INTERVAL_MS) {
    updateOdometer(g_currentMotion.left.direction, g_currentMotion.right.direction);
    sendOdometryData();
    g_lastOdomReportTime = currentTime;
  }

  // 2. 检查通信超时
  if (!g_emergencyStop && (currentTime - g_lastCmdTime) > COMMAND_TIMEOUT_MS) {
    // 超过1秒未收到命令，自动停止（任一电机非停即触发）
    if (g_currentMotion.left.direction != MotorDirection::STOP ||
        g_currentMotion.right.direction != MotorDirection::STOP) {
      g_currentMotion = VehicleMotion(
          MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                     MotorDirection::STOP, 0),
          MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                     MotorDirection::STOP, 0)
      );
      applyVehicleMotion(g_currentMotion);
#if DEBUG_MOTOR
      Serial.println("[超时] 自动停止");
#endif
    }
  }

  // 3. 小延迟，给 WiFi 后台任务足够 CPU 时间维持连接
  delay(5);
}
