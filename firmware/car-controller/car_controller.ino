/**
 * 智能车控制器主程序 - ESP32-C6
 * 基于函数式编程思想，使用Arduino IDE开发
 * 
 * 功能：
 * 1. 接收ESP-NOW无线命令（来自接收器）
 * 2. 控制L298N驱动4个电机
 * 3. 控制SG90舵机云台
 * 4. 发送状态反馈
 * 
 * 硬件接线：
 * - L298N #1: IN1->GPIO4, IN2->GPIO5, EN->GPIO6 (控制左侧电机)
 * - L298N #2: IN1->GPIO7, IN2->GPIO8, EN->GPIO9 (控制右侧电机)
 * - SG90水平: GPIO14
 * - SG90垂直: GPIO15
 * 
 * 作者：智能车项目团队
 * 版本：1.0.0
 */

#include "motor_control.h"
#include "servo_control.h"
#include "wireless.h"

// ============================================
// 全局状态（可变状态，在主循环中更新）
// ============================================

/**
 * 当前车辆运动状态
 * 每次命令更新时创建新状态
 */
VehicleMotion g_currentMotion = createStopState();

/**
 * 当前云台状态
 */
GimbalState g_currentGimbal = createInitialGimbalState();

/**
 * 当前速度值（0-255）
 */
uint8_t g_currentSpeed = 128;

/**
 * 最后命令接收时间
 */
uint32_t g_lastCmdTime = 0;

/**
 * 紧急停止标志
 */
bool g_emergencyStop = false;

// ============================================
// 命令处理函数
// ============================================

/**
 * 处理运动命令
 * 输入：WASD命令字符
 * 效果：更新车辆运动状态
 */
void handleMoveCommand(const char cmd) {
    // 解析命令并创建新状态
    g_currentMotion = parseWASDCommand(cmd, g_currentSpeed);
    
    // 应用状态到硬件
    applyVehicleMotion(g_currentMotion);
    
    // 更新时间戳
    g_lastCmdTime = millis();
    
    Serial.printf("[运动命令] 执行: %c, 速度: %d\n", cmd, g_currentSpeed);
}

/**
 * 处理舵机命令
 * 输入：云台命令字符
 * 效果：更新云台状态
 */
void handleServoCommand(const char cmd) {
    g_currentGimbal = parseGimbalCommand(g_currentGimbal, cmd);
    Serial.printf("[舵机命令] 执行: %c, 目标角度 H:%d V:%d\n", 
                  cmd, 
                  g_currentGimbal.horizontal.targetAngle,
                  g_currentGimbal.vertical.targetAngle);
}

/**
 * 处理速度命令
 * 输入：速度值
 */
void handleSpeedCommand(const uint8_t speed) {
    g_currentSpeed = speed;
    Serial.printf("[速度设置] 新速度: %d\n", g_currentSpeed);
}

/**
 * 处理停止命令
 */
void handleStopCommand() {
    g_currentMotion = createStopState();
    applyVehicleMotion(g_currentMotion);
    g_emergencyStop = true;
    Serial.println("[紧急停止] 车辆已停止");
}

// ============================================
// ESP-NOW 接收回调
// ============================================

void onDataRecv(const uint8_t* mac, const uint8_t* incomingData, int len) {
    if (len != sizeof(WirelessPacket)) {
        return;
    }
    
    const WirelessPacket* packet = reinterpret_cast<const WirelessPacket*>(incomingData);
    
    if (!validatePacket(*packet)) {
        Serial.println("[无线通信] 收到无效数据包");
        return;
    }
    
    // 处理命令
    switch (packet->type) {
        case CommandType::MOVE:
            handleMoveCommand(static_cast<char>(packet->data));
            break;
        case CommandType::SERVO:
            handleServoCommand(static_cast<char>(packet->data));
            break;
        case CommandType::SPEED:
            handleSpeedCommand(packet->speed);
            break;
        case CommandType::STOP:
            handleStopCommand();
            break;
        case CommandType::STATUS:
            // 发送状态反馈
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
    Serial.println("版本: 1.0.0");
    Serial.println("================================\n");
    
    // 初始化电机引脚
    initializeMotorPins();
    delay(100);
    
    // 初始化舵机引脚
    initializeServoPins();
    delay(100);
    
    // 初始化无线通信
    if (!initializeWireless(DeviceRole::CAR)) {
        Serial.println("[初始化] 无线通信初始化失败，重启中...");
        delay(1000);
        ESP.restart();
    }
    
    // 注册接收回调
    esp_now_register_recv_cb(onDataRecv);
    
    // 初始化状态
    g_currentMotion = createStopState();
    g_currentGimbal = createInitialGimbalState();
    g_currentSpeed = 128;
    g_emergencyStop = false;
    
    Serial.println("[初始化] 系统启动完成，等待命令...");
    Serial.println("[命令说明] WASD:移动, Q/E:原地旋转, U/D/L/R:云台, 空格:停止, 1-9:速度");
}

// ============================================
// 主循环
// ============================================

void loop() {
    const uint32_t currentTime = millis();
    
    // 1. 更新云台（平滑移动）
    g_currentGimbal = updateGimbal(g_currentGimbal);
    
    // 2. 检查通信超时
    if (!g_emergencyStop && (currentTime - g_lastCmdTime) > 1000) {
        // 超过1秒未收到命令，自动停止
        if (g_currentMotion.frontLeft.direction != MotorDirection::STOP) {
            g_currentMotion = createStopState();
            applyVehicleMotion(g_currentMotion);
            Serial.println("[超时] 自动停止");
        }
    }
    
    // 3. 检查紧急停止恢复
    if (g_emergencyStop && (currentTime - g_lastCmdTime) < 500) {
        g_emergencyStop = false;
    }
    
    // 4. 小延迟，避免占用过多CPU
    delay(10);
}
