/**
 * 舵机控制系统 - 函数式编程风格
 * 基于 ESP32-C6，使用 SG90 舵机
 * 用于控制摄像头云台或机械臂
 * 作者：智能车项目团队
 * 版本：1.2.0
 */

#ifndef SERVO_CONTROL_H
#define SERVO_CONTROL_H

#include <Arduino.h>

// ============================================
// 纯数据类型定义
// ============================================

/**
 * 舵机配置结构体
 * 包含所有舵机相关的常量参数
 */
struct ServoConfig {
    const uint8_t pin;           // 控制引脚
    const uint8_t minAngle;      // 最小角度（防卡死）
    const uint8_t maxAngle;      // 最大角度（防卡死）
    const uint16_t minPulse;     // 最小脉宽（微秒）
    const uint16_t maxPulse;     // 最大脉宽（微秒）
    const uint8_t defaultAngle;  // 默认初始角度
    
    constexpr ServoConfig(
        uint8_t p, uint8_t minA, uint8_t maxA,
        uint16_t minP, uint16_t maxP, uint8_t defA
    ) : pin(p), minAngle(minA), maxAngle(maxA),
        minPulse(minP), maxPulse(maxP), defaultAngle(defA) {}
};

/**
 * 舵机状态结构体
 * 记录当前角度和目标角度（支持平滑移动）
 */
struct ServoState {
    const uint8_t currentAngle;   // 当前角度
    const uint8_t targetAngle;    // 目标角度
    const uint8_t speed;          // 移动速度（每步角度变化）
    const bool isMoving;          // 是否正在移动
    
    constexpr ServoState(
        uint8_t curr, uint8_t tgt, uint8_t spd, bool moving
    ) : currentAngle(curr), targetAngle(tgt), speed(spd), isMoving(moving) {}
};

/**
 * 云台状态（双轴）
 */
struct GimbalState {
    const ServoState horizontal;  // 水平舵机
    const ServoState vertical;    // 垂直舵机
    
    constexpr GimbalState(ServoState h, ServoState v)
        : horizontal(h), vertical(v) {}
};

// ============================================
// 硬件配置常量
// ============================================
namespace ServoPinConfig {
    // 水平舵机引脚（控制左右转动）
    constexpr uint8_t HORIZONTAL_SERVO_PIN = 14;
    // 垂直舵机引脚（控制上下转动）
    constexpr uint8_t VERTICAL_SERVO_PIN = 15;
}

// SG90 标准配置
namespace SG90Config {
    constexpr uint8_t MIN_ANGLE = 0;      // 最小角度
    constexpr uint8_t MAX_ANGLE = 180;    // 最大角度
    constexpr uint16_t MIN_PULSE = 500;   // 最小脉宽 0.5ms
    constexpr uint16_t MAX_PULSE = 2400;  // 最大脉宽 2.4ms
    constexpr uint8_t DEFAULT_ANGLE = 90; // 默认角度（正中）
    constexpr uint8_t DEFAULT_SPEED = 2;  // 默认移动速度
}

// ============================================
// 纯函数：舵机核心逻辑
// ============================================

/**
 * 纯函数：创建默认舵机配置
 */
inline ServoConfig createDefaultServoConfig(uint8_t pin) {
    return ServoConfig(
        pin,
        SG90Config::MIN_ANGLE,
        SG90Config::MAX_ANGLE,
        SG90Config::MIN_PULSE,
        SG90Config::MAX_PULSE,
        SG90Config::DEFAULT_ANGLE
    );
}

/**
 * 纯函数：角度限制（防超出安全范围）
 * 输入：目标角度
 * 输出：限制后的角度
 */
inline uint8_t clampAngle(const uint8_t angle, const ServoConfig& config) {
    if (angle < config.minAngle) return config.minAngle;
    if (angle > config.maxAngle) return config.maxAngle;
    return angle;
}

/**
 * 纯函数：角度转脉宽（线性映射）
 * 将角度映射到对应的PWM脉宽（微秒）
 * 输入：角度，配置
 * 输出：脉宽（微秒）
 */
inline uint16_t angleToPulse(const uint8_t angle, const ServoConfig& config) {
    const uint8_t clamped = clampAngle(angle, config);
    // 线性映射：角度 -> 脉宽
    return map(clamped, 
                 config.minAngle, config.maxAngle,
                 config.minPulse, config.maxPulse);
}

/**
 * 纯函数：计算下一步角度（平滑移动）
 * 输入：当前状态
 * 输出：新状态
 */
inline ServoState calculateNextStep(const ServoState& current) {
    if (!current.isMoving || current.currentAngle == current.targetAngle) {
        return ServoState(
            current.currentAngle,
            current.targetAngle,
            current.speed,
            false
        );
    }
    
    // 计算移动方向
    int8_t direction = 0;
    if (current.targetAngle > current.currentAngle) {
        direction = 1;
    } else if (current.targetAngle < current.currentAngle) {
        direction = -1;
    }
    
    // 计算下一步角度
    int16_t nextAngle = current.currentAngle + (direction * current.speed);
    
    // 检查是否到达目标
    bool stillMoving = true;
    if ((direction > 0 && nextAngle >= current.targetAngle) ||
        (direction < 0 && nextAngle <= current.targetAngle)) {
        nextAngle = current.targetAngle;
        stillMoving = false;
    }
    
    return ServoState(
        static_cast<uint8_t>(nextAngle),
        current.targetAngle,
        current.speed,
        stillMoving
    );
}

/**
 * 纯函数：设置舵机目标角度
 * 输入：当前状态，目标角度
 * 输出：新状态
 */
inline ServoState setTargetAngle(const ServoState& current, const uint8_t target, const ServoConfig& config) {
    const uint8_t clamped = clampAngle(target, config);
    return ServoState(
        current.currentAngle,
        clamped,
        current.speed,
        true  // 标记为需要移动
    );
}

/**
 * 纯函数：设置舵机速度
 */
inline ServoState setServoSpeed(const ServoState& current, const uint8_t speed) {
    return ServoState(
        current.currentAngle,
        current.targetAngle,
        speed,
        current.isMoving
    );
}

/**
 * 纯函数：创建初始舵机状态
 */
inline ServoState createInitialServoState(uint8_t angle) {
    return ServoState(angle, angle, SG90Config::DEFAULT_SPEED, false);
}

/**
 * 纯函数：创建初始云台状态
 */
inline GimbalState createInitialGimbalState() {
    return GimbalState(
        createInitialServoState(SG90Config::DEFAULT_ANGLE),
        createInitialServoState(SG90Config::DEFAULT_ANGLE)
    );
}

// ============================================
// 硬件控制函数（包含副作用）
// ============================================

/**
 * 初始化舵机引脚
 */
inline void initializeServoPins() {
    pinMode(ServoPinConfig::HORIZONTAL_SERVO_PIN, OUTPUT);
    pinMode(ServoPinConfig::VERTICAL_SERVO_PIN, OUTPUT);
    
    // 初始化到默认位置
    const ServoConfig hConfig = createDefaultServoConfig(ServoPinConfig::HORIZONTAL_SERVO_PIN);
    const ServoConfig vConfig = createDefaultServoConfig(ServoPinConfig::VERTICAL_SERVO_PIN);
    
    // 输出初始PWM信号
    const uint16_t hPulse = angleToPulse(SG90Config::DEFAULT_ANGLE, hConfig);
    const uint16_t vPulse = angleToPulse(SG90Config::DEFAULT_ANGLE, vConfig);
    
    // 使用ESP32的LEDC库输出PWM
    // 注：实际实现需使用ledcWrite
    ledcAttach(ServoPinConfig::HORIZONTAL_SERVO_PIN, 50, 16); // 50Hz, 16位
    ledcAttach(ServoPinConfig::VERTICAL_SERVO_PIN, 50, 16);
    
    ledcWrite(ServoPinConfig::HORIZONTAL_SERVO_PIN, hPulse * 65535 / 20000);
    ledcWrite(ServoPinConfig::VERTICAL_SERVO_PIN, vPulse * 65535 / 20000);
    
    Serial.println("[舵机控制] 引脚初始化完成");
}

/**
 * 应用舵机状态到硬件
 * 输入：舵机状态，配置
 */
inline void applyServoState(const ServoState& state, const ServoConfig& config) {
    const uint16_t pulse = angleToPulse(state.currentAngle, config);
    // 将微秒转换为duty cycle（16位分辨率）
    const uint32_t duty = (pulse * 65535ULL) / 20000; // 20ms周期
    ledcWrite(config.pin, duty);
}

/**
 * 更新云台（含平滑移动）
 * 输入：当前云台状态
 * 输出：新云台状态
 */
inline GimbalState updateGimbal(const GimbalState& current) {
    const ServoState nextH = calculateNextStep(current.horizontal);
    const ServoState nextV = calculateNextStep(current.vertical);
    
    // 应用新状态
    const ServoConfig hConfig = createDefaultServoConfig(ServoPinConfig::HORIZONTAL_SERVO_PIN);
    const ServoConfig vConfig = createDefaultServoConfig(ServoPinConfig::VERTICAL_SERVO_PIN);
    
    applyServoState(nextH, hConfig);
    applyServoState(nextV, vConfig);
    
    return GimbalState(nextH, nextV);
}

/**
 * 纯函数：解析云台控制命令
 * 支持：U(上), D(下), L(左), R(右), C(居中)
 */
inline GimbalState parseGimbalCommand(const GimbalState& current, const char cmd) {
    const uint8_t step = 5;  // 每步移动角度
    
    uint8_t newHAngle = current.horizontal.targetAngle;
    uint8_t newVAngle = current.vertical.targetAngle;
    
    switch (cmd) {
        case 'U': case 'u':
            // 安全加法：防止超出最大角度
            newVAngle = (newVAngle + step <= SG90Config::MAX_ANGLE)
                        ? newVAngle + step : SG90Config::MAX_ANGLE;
            break;
        case 'D': case 'd':
            // 安全减法：防止 uint8_t 下溢（0 - 5 = 251）
            newVAngle = (newVAngle >= step) ? newVAngle - step : 0;
            break;
        case 'H': case 'h':
            // 安全减法：防止 uint8_t 下溢（云台左转）
            newHAngle = (newHAngle >= step) ? newHAngle - step : 0;
            break;
        case 'K': case 'k':
            // 安全加法：防止超出最大角度（云台右转）
            newHAngle = (newHAngle + step <= SG90Config::MAX_ANGLE)
                        ? newHAngle + step : SG90Config::MAX_ANGLE;
            break;
        case 'C': case 'c':
            newHAngle = SG90Config::DEFAULT_ANGLE;
            newVAngle = SG90Config::DEFAULT_ANGLE;
            break;
    }
    
    // 限制角度范围
    const ServoConfig hConfig = createDefaultServoConfig(ServoPinConfig::HORIZONTAL_SERVO_PIN);
    const ServoConfig vConfig = createDefaultServoConfig(ServoPinConfig::VERTICAL_SERVO_PIN);
    
    newHAngle = clampAngle(newHAngle, hConfig);
    newVAngle = clampAngle(newVAngle, vConfig);
    
    return GimbalState(
        setTargetAngle(current.horizontal, newHAngle, hConfig),
        setTargetAngle(current.vertical, newVAngle, vConfig)
    );
}

#endif // SERVO_CONTROL_H
