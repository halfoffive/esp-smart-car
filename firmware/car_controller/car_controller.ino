/**
 * 智能车控制器主程序 - ESP32-S3（Freenove FNK0085 单芯片架构）
 * 基于函数式编程思想，使用Arduino IDE开发
 * 
 * 功能：
 * 1. 接收ESP-NOW无线命令（来自接收器）
 * 2. 控制L298N驱动4个电机
 * 3. 测速模块：编码器读取+速度计算
 * 4. PID控制：直线修正+精确方向
 * 5. 发送状态反馈（含测速数据）
 * 6. 摄像头采集视频帧并通过 ESP-NOW 直发到接收器（S3 单芯片，无 Serial1 桥接）
 * 7. BLE 广播（让接收器可扫描到本机 MAC，Manufacturer Data 嵌入 WiFi MAC）
 *
 * 硬件接线（ESP32-S3 WROOM CAM，Freenove FNK0085）：
 * - 摄像头（OV2640）: GPIO 4-18（除 GPIO 14 外，均为摄像头专用引脚）
 * - L298N #1: IN1->GPIO38, IN2->GPIO39, EN->GPIO40 (控制左侧电机)
 * - L298N #2: IN1->GPIO41, IN2->GPIO42, EN->GPIO21 (控制右侧电机)
 * - 左编码器: GPIO1（中断引脚）
 * - 右编码器: GPIO2（中断引脚）
 * 
 * 作者：智能车项目团队
 * 版本：1.5.0（S3 单芯片整合，砍除 C6 + Serial1 桥接）
 */

#include "motor_control.h"
#include "../libraries/wireless_protocol/src/wireless.h"
#include "odometer.h"
#include "pid_control.h"
#include "camera_config.h"
#include "video_stream.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>

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
VehicleMotion g_currentMotion = createStopState();

/**
 * 当前速度值（0-255）
 * 初始值 141 对应前端默认速度等级 5（接收器 map(5, 1, 9, 28, 255) = 141）
 * 避免首次连接未发送速度命令前车速与前端显示不一致
 */
uint8_t g_currentSpeed = 141;

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

/**
 * 命令超时自动停止时间(ms)
 * 超过此时间未收到任何有效命令（运动/速度/心跳/行走模式）则自动停车
 */
constexpr uint32_t COMMAND_TIMEOUT_MS = 1000;
uint32_t g_lastOdomReportTime = 0;

/**
 * 直线修正使能标志
 */
bool g_smartDriveEnabled = false;

/**
 * 摄像头配置（运行时复用，错误恢复时重新初始化）
 */
CameraConfiguration g_cameraConfig = createDefaultConfig();

// ============================================
// 编码器中断服务函数（定义在此处而非 odometer.h，避免 inline + IRAM_ATTR 导致的 literal pool 重定位错误）
// ============================================

void IRAM_ATTR onLeftEncoderPulse() {
    OdometerState::g_leftPulses += 1;
}

void IRAM_ATTR onRightEncoderPulse() {
    OdometerState::g_rightPulses += 1;
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
  // 创建基础运动状态
  g_currentMotion = parseWASDCommand(cmd, g_currentSpeed);

  // 如果启用智能修正，应用PID修正
  if (g_smartDriveEnabled && cmd != ' ') {
    // 获取当前运动方向
    MotorDirection leftDir = g_currentMotion.left.direction;
    MotorDirection rightDir = g_currentMotion.right.direction;

    // 只有前后运动才做直线修正（转弯不需要）
    if ((leftDir == MotorDirection::FORWARD && rightDir == MotorDirection::FORWARD) || (leftDir == MotorDirection::BACKWARD && rightDir == MotorDirection::BACKWARD)) {

      // 更新测速数据
      updateOdometer();

      // 应用PID智能修正
      SmartMotorOutput output = updateSmartControl(
        g_currentSpeed, leftDir, rightDir);

      // 创建修正后的差速运动状态
      g_currentMotion = createDifferentialState(
        output.leftDir, output.leftPwm,
        output.rightDir, output.rightPwm);
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
    INT16_MIN, INT16_MAX));
  const int16_t rightSpeed = static_cast<int16_t>(constrain(
    static_cast<long>(odom.rightWheel.mmps),
    INT16_MIN, INT16_MAX));
  const int16_t headingX100 = static_cast<int16_t>(constrain(
    static_cast<long>(odom.heading * 100.0f),
    INT16_MIN, INT16_MAX));
  const uint16_t totalDist = static_cast<uint16_t>(
    fmin(odom.distanceMm, 65535.0f));

  // 创建测速数据包
  OdometryPacket packet(
    WirelessConfig::MAGIC_BYTE,
    WirelessConfig::PROTOCOL_VERSION,
    CommandType::ODOMETRY,
    leftSpeed,
    rightSpeed,
    headingX100,
    totalDist,
    0  // 校验和暂填0
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
    packet.headingX100, packet.totalDistMm, checksum);

  // 发送到接收器
  // 注意：直接发送 OdometryPacket（12字节），通过通用发送函数避免 reinterpret_cast UB
  sendRawPacket(WirelessConfig::RECEIVER_MAC,
                reinterpret_cast<const uint8_t*>(&finalPacket),
                sizeof(finalPacket));
}

// ============================================
// 摄像头视频帧采集与 ESP-NOW 直发
// ============================================

/**
 * 采集一帧视频并通过 ESP-NOW 分包发送到接收器
 * 包含帧率控制（30 FPS = 33ms 间隔）、错误恢复（连续 10 次失败重启摄像头）、
 * 动态质量调整（根据帧大小自适应 JPEG 压缩率）
 * 
 * 返回：true 表示本次循环已处理帧（无论成功或失败），false 表示未到帧间隔
 */
bool captureAndSendVideoFrame() {
  const uint32_t currentTime = millis();

  // 帧率控制：30 FPS = 33ms 间隔
  if (currentTime - g_streamState.lastFrameTime < VideoStreamConfig::FRAME_INTERVAL) {
    return false;
  }

  // 捕获帧
  const FrameState frame = captureFrame();
  if (!frame.isValid) {
    // 帧捕获失败，更新丢弃计数
    g_streamState = StreamState(
      true, g_streamState.lastFrameTime, g_streamState.fps,
      g_streamState.totalFrames, g_streamState.droppedFrames + 1,
      g_streamState.bytesSent);
    // 连续失败恢复逻辑：超过阈值时重启摄像头硬件
    g_consecutiveFailures++;
    if (g_consecutiveFailures >= CAMERA_RESTART_THRESHOLD) {
      Serial.printf("[视频流] 连续 %d 次帧捕获失败，重启摄像头...\n",
                    g_consecutiveFailures);
      esp_camera_deinit();
      delay(500);
      if (!initializeCamera(g_cameraConfig)) {
        Serial.println("[视频流] 摄像头重启失败，继续重试...");
      } else {
        Serial.println("[视频流] 摄像头重启成功");
      }
      g_consecutiveFailures = 0;
    }
    return true;
  }

  // 帧捕获成功，重置连续失败计数
  g_consecutiveFailures = 0;

  // 通过 ESP-NOW 分包发送到接收器（S3 单芯片直发，无 Serial1 桥接）
  sendVideoFrame(frame);

  // 动态调整质量（根据帧大小自适应压缩率）
  const uint8_t newQuality = adjustQuality(frame.frameSize);
  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != NULL) {
    sensor->set_quality(sensor, newQuality);
  }

  // 更新流状态
  const uint16_t fps = calculateFPS(g_streamState.lastFrameTime, currentTime);
  g_streamState = StreamState(
    true, currentTime, fps,
    g_streamState.totalFrames + 1, g_streamState.droppedFrames,
    g_streamState.bytesSent + frame.frameSize);

  // 释放帧缓冲
  releaseFrame(frame);

  // 每100帧打印一次统计
  static uint32_t lastLoggedFrame = 0;
  const StreamState state = getStreamState();
  if (state.totalFrames != lastLoggedFrame && state.totalFrames % 100 == 0 && state.totalFrames > 0) {
    lastLoggedFrame = state.totalFrames;
    Serial.printf("[视频流] FPS:%d, 总帧:%lu, 丢弃:%lu, 发送:%lu KB\n",
                  state.fps,
                  (unsigned long)state.totalFrames,
                  (unsigned long)state.droppedFrames,
                  (unsigned long)(state.bytesSent / 1024));
  }

  return true;
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
  Serial.println("智能车控制系统 - ESP32-S3（Freenove FNK0085）");
  Serial.println("版本: 1.5.0 (S3 单芯片：摄像头+电机+编码器+PID+ESP-NOW+BLE)");
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

  // 初始化无线通信（ESP-NOW）
  if (!initializeWireless(DeviceRole::CAR)) {
    Serial.println("[初始化] 无线通信初始化失败，重启中...");
    delay(1000);
    ESP.restart();
  }

  // 注册接收回调
  if (esp_now_register_recv_cb(onDataRecv) != ESP_OK) {
    Serial.println("[无线通信] 注册接收回调失败");
  }

  // 打印 ESP-NOW 通信用的 WiFi MAC（与 BLE MAC 不同，必须区分）
  uint8_t wifiMacBytes[6];
  WiFi.macAddress(wifiMacBytes);  // 获取原始 6 字节 WiFi MAC
  Serial.print("[初始化] ESP-NOW MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("[初始化] ⚠ 此 MAC 用于 ESP-NOW 连接，与 BLE 扫描显示的 MAC 不同");

  // 初始化 BLE 设备并启动广播（让接收器可扫描到本机）
  BLEDevice::init("智能车");
  BLEServer* pServer = BLEDevice::createServer();  // 创建 BLE 服务器
  (void)pServer;                                   // 仅需存在即可，后续不需要引用
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  // 将 WiFi MAC 嵌入 BLE 广播的 Manufacturer Data 中
  // 格式: [Company ID 2字节=0xFFFF] + [WiFi MAC 6字节] = 共 8 字节
  // 接收器扫描时可提取 WiFi MAC，用于 ESP-NOW 连接配置
  // NimBLE API: 通过 BLEAdvertisementData 设置 manufacturer data（Arduino String）
  BLEAdvertisementData oAdvertisementData;
  String mfgStr;
  mfgStr += char(0xFF);  // Company ID 低字节（0xFFFF = 测试用）
  mfgStr += char(0xFF);  // Company ID 高字节
  for (int i = 0; i < 6; i++) mfgStr += char(wifiMacBytes[i]);
  oAdvertisementData.setManufacturerData(mfgStr);
  pAdvertising->setAdvertisementData(oAdvertisementData);
  pAdvertising->start();  // 开始广播
  Serial.println("[初始化] BLE 广播已启动 (设备名: 智能车, 含 WiFi MAC)");

  // 初始化摄像头（S3 单芯片架构：摄像头与电机/编码器/PID 共用同一 MCU）
  if (!initializeCamera(g_cameraConfig)) {
    Serial.println("[摄像头] 初始化失败，重启中...");
    delay(2000);
    ESP.restart();
  }

  // 启动视频流（标记流状态为活跃，loop() 中按 30 FPS 采集发送）
  startStreaming();
  Serial.println("[初始化] 摄像头视频流已启动（ESP-NOW 直发接收器）");

  // 初始化状态
  g_currentMotion = createStopState();
  // 与前端默认速度等级 5 对应（接收器 map(5, 1, 9, 28, 255) = 141）
  g_currentSpeed = 141;
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

  // 1. 采集摄像头视频帧并通过 ESP-NOW 直发到接收器（30 FPS = 33ms 间隔）
  //    包含错误恢复（连续 10 次失败重启摄像头）和动态质量调整
  (void)captureAndSendVideoFrame();

  // 2. 定期更新测速数据并发送（与采样周期对齐，避免无效调用）
  if (currentTime - g_lastOdomReportTime >= ODOMETRY_REPORT_INTERVAL_MS) {
    updateOdometer();
    sendOdometryData();
    g_lastOdomReportTime = currentTime;
  }

  // 3. 检查通信超时
  if (!g_emergencyStop && (currentTime - g_lastCmdTime) > COMMAND_TIMEOUT_MS) {
    // 超过1秒未收到命令，自动停止
    if (g_currentMotion.left.direction != MotorDirection::STOP) {
      g_currentMotion = createStopState();
      applyVehicleMotion(g_currentMotion);
#if DEBUG_MOTOR
      Serial.println("[超时] 自动停止");
#endif
    }
  }

  // 4. 小延迟，避免占用过多CPU
  delay(1);
}
