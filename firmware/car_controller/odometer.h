/**
 * 测速模块 - 函数式编程风格
 * 基于 ESP32-C6，使用霍尔编码器或红外编码器测量轮速
 * 
 * 功能：
 * 1. 中断方式读取编码器脉冲
 * 2. 计算左右轮实时速度 (RPM)
 * 3. 计算行走距离和加速度
 * 4. 提供测速数据供PID控制器使用
 * 
 * 硬件：
 * - 左轮编码器: GPIO 0 (中断引脚)
 * - 右轮编码器: GPIO 1 (中断引脚)
 * - 编码器: 霍尔传感器或红外对管，每圈N个脉冲
 * 
 * 作者：智能车项目团队
 * 版本：1.2.0
 */

#ifndef ODOMETER_H
#define ODOMETER_H

#include <Arduino.h>

// ============================================
// 纯数据类型定义
// ============================================

/**
 * 编码器配置
 * 记录编码器的硬件参数
 */
struct EncoderConfig {
    uint8_t pin;              // 中断引脚
    uint8_t pulsesPerRev;     // 每圈脉冲数（编码器线数）
    float wheelDiameter;       // 轮子直径(mm)
    float gearRatio;           // 减速比（电机转速:轮速）
    
    constexpr EncoderConfig(
        uint8_t p, uint8_t ppr, float wd, float gr
    ) : pin(p), pulsesPerRev(ppr), wheelDiameter(wd), gearRatio(gr) {}
};

/**
 * 单轮测速数据
 * 所有字段均为 const，确保不可变性
 */
struct WheelSpeed {
    float rpm;                // 转速(RPM)
    float mmps;               // 线速度(mm/s)
    float distanceMm;         // 累计行走距离(mm)
    uint32_t pulseCount;      // 脉冲计数
    uint32_t timestampMs;     // 时间戳(ms)
    
    constexpr WheelSpeed(
        float r, float m, float d, uint32_t pc, uint32_t ts
    ) : rpm(r), mmps(m), distanceMm(d), pulseCount(pc), timestampMs(ts) {}
};

/**
 * 整车测速数据
 * 包含左右轮速度和整车状态
 */
struct OdometryData {
    WheelSpeed leftWheel;     // 左轮数据
    WheelSpeed rightWheel;    // 右轮数据
    float linearSpeed;        // 整车线速度(mm/s)
    float angularVelocity;    // 角速度(rad/s)
    float heading;            // 航向角(弧度)
    float distanceMm;         // 整车行走距离(mm)
    uint32_t timestampMs;     // 时间戳(ms)
    
    constexpr OdometryData(
        WheelSpeed lw, WheelSpeed rw,
        float ls, float av, float hd, float dm, uint32_t ts
    ) : leftWheel(lw), rightWheel(rw),
        linearSpeed(ls), angularVelocity(av),
        heading(hd), distanceMm(dm), timestampMs(ts) {}
};

/**
 * 测速校准参数
 * 用于补偿左右轮速度差异
 */
struct SpeedCalibration {
    float leftCorrection;     // 左轮修正系数(>1加速, <1减速)
    float rightCorrection;    // 右轮修正系数
    
    constexpr SpeedCalibration(float lc, float rc)
        : leftCorrection(lc), rightCorrection(rc) {}
};

// ============================================
// 引脚和常量配置
// ============================================
namespace OdometerConfig {
    // 编码器引脚（GPIO 0 和 GPIO 1 支持中断）
    constexpr uint8_t LEFT_ENCODER_PIN = 0;
    constexpr uint8_t RIGHT_ENCODER_PIN = 1;
    
    // 编码器参数（根据实际硬件调整）
    constexpr uint8_t PULSES_PER_REV = 20;       // 每圈脉冲数
    constexpr float WHEEL_DIAMETER_MM = 65.0f;   // 轮子直径65mm
    constexpr float GEAR_RATIO = 1.0f;           // 减速比（直驱为1）
    constexpr float WHEEL_BASE_MM = 150.0f;       // 轮距150mm
    
    // 计算参数
    constexpr float WHEEL_CIRCUMFERENCE_MM = 3.14159265f * WHEEL_DIAMETER_MM;
    constexpr float MM_PER_PULSE = WHEEL_CIRCUMFERENCE_MM / PULSES_PER_REV;
    
    // 速度计算采样周期(ms)
    constexpr uint16_t SAMPLE_PERIOD_MS = 100;   // 100ms采样一次
    
    // 默认校准参数（1.0 = 无修正）
    constexpr SpeedCalibration DEFAULT_CALIBRATION = SpeedCalibration(1.0f, 1.0f);
}

// ============================================
// 可变全局状态（中断修改，主循环读取）
// ============================================
namespace OdometerState {
    // 脉冲计数（volatile 用于中断安全）
    volatile uint32_t g_leftPulses = 0;
    volatile uint32_t g_rightPulses = 0;
    uint32_t g_lastLeftPulses = 0;    // 非 ISR 变量，仅主循环访问
    uint32_t g_lastRightPulses = 0;   // 非 ISR 变量，仅主循环访问
    
    // 累计距离
    float g_leftDistanceMm = 0.0f;
    float g_rightDistanceMm = 0.0f;
    
    // 速度计算
    float g_leftSpeedMmps = 0.0f;
    float g_rightSpeedMmps = 0.0f;
    float g_leftRpm = 0.0f;
    float g_rightRpm = 0.0f;
    
    // 航向
    float g_heading = 0.0f;
    float g_totalDistanceMm = 0.0f;
    
    // 时间
    uint32_t g_lastSampleTime = 0;
    
    // 校准
    SpeedCalibration g_calibration = OdometerConfig::DEFAULT_CALIBRATION;
}

// ============================================
// 中断服务函数
// ============================================

/**
 * 左轮编码器中断
 * 每检测到一个脉冲递增计数
 */
inline void IRAM_ATTR onLeftEncoderPulse() {
    OdometerState::g_leftPulses += 1;
}

/**
 * 右轮编码器中断
 * 每检测到一个脉冲递增计数
 */
inline void IRAM_ATTR onRightEncoderPulse() {
    OdometerState::g_rightPulses += 1;
}

// ============================================
// 初始化函数
// ============================================

/**
 * 初始化编码器引脚和中断
 * 副作用：配置GPIO和中断
 */
inline void initializeOdometer() {
    pinMode(OdometerConfig::LEFT_ENCODER_PIN, INPUT_PULLUP);
    pinMode(OdometerConfig::RIGHT_ENCODER_PIN, INPUT_PULLUP);
    
    attachInterrupt(digitalPinToInterrupt(OdometerConfig::LEFT_ENCODER_PIN), 
                    onLeftEncoderPulse, RISING);
    attachInterrupt(digitalPinToInterrupt(OdometerConfig::RIGHT_ENCODER_PIN), 
                    onRightEncoderPulse, RISING);
    
    OdometerState::g_leftPulses = 0;
    OdometerState::g_rightPulses = 0;
    OdometerState::g_lastLeftPulses = 0;
    OdometerState::g_lastRightPulses = 0;
    OdometerState::g_leftDistanceMm = 0.0f;
    OdometerState::g_rightDistanceMm = 0.0f;
    OdometerState::g_leftSpeedMmps = 0.0f;
    OdometerState::g_rightSpeedMmps = 0.0f;
    OdometerState::g_leftRpm = 0.0f;
    OdometerState::g_rightRpm = 0.0f;
    OdometerState::g_heading = 0.0f;
    OdometerState::g_totalDistanceMm = 0.0f;
    OdometerState::g_lastSampleTime = millis();
    OdometerState::g_calibration = OdometerConfig::DEFAULT_CALIBRATION;
    
    Serial.println("[测速模块] 编码器初始化完成");
    Serial.printf("  左轮引脚: GPIO %d\n", OdometerConfig::LEFT_ENCODER_PIN);
    Serial.printf("  右轮引脚: GPIO %d\n", OdometerConfig::RIGHT_ENCODER_PIN);
    Serial.printf("  每圈脉冲: %d\n", OdometerConfig::PULSES_PER_REV);
    Serial.printf("  轮子直径: %.1f mm\n", OdometerConfig::WHEEL_DIAMETER_MM);
    Serial.printf("  轮距: %.1f mm\n", OdometerConfig::WHEEL_BASE_MM);
}

// ============================================
// 纯函数：速度计算
// ============================================

/**
 * 纯函数：根据脉冲差计算速度
 * 输入：脉冲数差值，采样周期(ms)
 * 输出：线速度(mm/s)
 */
inline float calculateSpeedMmps(uint32_t pulseDelta, uint32_t periodMs) {
    if (periodMs == 0) return 0.0f;
    const float distanceMm = static_cast<float>(pulseDelta) * OdometerConfig::MM_PER_PULSE;
    return distanceMm * 1000.0f / static_cast<float>(periodMs);
}

/**
 * 纯函数：计算转速
 * 输入：脉冲数差值，采样周期(ms)
 * 输出：转速(RPM)
 */
inline float calculateRpm(uint32_t pulseDelta, uint32_t periodMs) {
    if (periodMs == 0) return 0.0f;
    const float revolutions = static_cast<float>(pulseDelta) / OdometerConfig::PULSES_PER_REV;
    const float periodMin = static_cast<float>(periodMs) / 60000.0f;
    return revolutions / periodMin;
}

/**
 * 纯函数：根据左右轮速度差计算角速度
 * 输入：左轮速度(mm/s)，右轮速度(mm/s)，轮距(mm)
 * 输出：角速度(rad/s)
 */
inline float calculateAngularVelocity(float leftMmps, float rightMmps) {
    return (rightMmps - leftMmps) / OdometerConfig::WHEEL_BASE_MM;
}

/**
 * 纯函数：根据角速度更新航向角
 * 输入：当前航向(弧度)，角速度(rad/s)，时间间隔(s)
 * 输出：新航向(弧度)
 */
inline float updateHeading(float heading, float angularVelocity, float dtSec) {
    // 更新航向角并归一化到 [0, 2π)，防止持续累加导致 int16_t 溢出
    float newHeading = heading + angularVelocity * dtSec;
    const float TWO_PI = 2.0f * M_PI;
    // fmod 处理负数时结果可能为负，需要额外调整
    newHeading = fmod(newHeading, TWO_PI);
    if (newHeading < 0.0f) {
        newHeading += TWO_PI;
    }
    return newHeading;
}

// ============================================
// 更新函数：在主循环中调用
// ============================================

/**
 * 更新测速数据
 * 在主循环中定期调用（建议100ms周期）
 * 副作用：更新全局测速状态
 */
inline void updateOdometer() {
    const uint32_t now = millis();
    const uint32_t elapsed = now - OdometerState::g_lastSampleTime;
    
    // 防止除零和采样过快
    if (elapsed < OdometerConfig::SAMPLE_PERIOD_MS) {
        return;
    }
    
    // 读取脉冲计数（关中断防止竞态）
    noInterrupts();
    const uint32_t leftPulses = OdometerState::g_leftPulses;
    const uint32_t rightPulses = OdometerState::g_rightPulses;
    interrupts();
    
    // 计算脉冲差值
    const uint32_t leftDelta = leftPulses - OdometerState::g_lastLeftPulses;
    const uint32_t rightDelta = rightPulses - OdometerState::g_lastRightPulses;
    
    // 更新上次脉冲计数
    OdometerState::g_lastLeftPulses = leftPulses;
    OdometerState::g_lastRightPulses = rightPulses;
    
    // 计算速度（应用校准系数）
    const float leftSpeed = calculateSpeedMmps(leftDelta, elapsed) 
                          * OdometerState::g_calibration.leftCorrection;
    const float rightSpeed = calculateSpeedMmps(rightDelta, elapsed)
                           * OdometerState::g_calibration.rightCorrection;
    
    OdometerState::g_leftSpeedMmps = leftSpeed;
    OdometerState::g_rightSpeedMmps = rightSpeed;
    OdometerState::g_leftRpm = calculateRpm(leftDelta, elapsed) 
                               * OdometerState::g_calibration.leftCorrection;
    OdometerState::g_rightRpm = calculateRpm(rightDelta, elapsed)
                               * OdometerState::g_calibration.rightCorrection;
    
    // 累计距离
    const float leftDistDelta = static_cast<float>(leftDelta) * OdometerConfig::MM_PER_PULSE;
    const float rightDistDelta = static_cast<float>(rightDelta) * OdometerConfig::MM_PER_PULSE;
    OdometerState::g_leftDistanceMm += leftDistDelta;
    OdometerState::g_rightDistanceMm += rightDistDelta;
    
    // 计算整车距离（取平均值）
    OdometerState::g_totalDistanceMm += (leftDistDelta + rightDistDelta) / 2.0f;
    
    // 计算角速度和航向
    const float angularVel = calculateAngularVelocity(leftSpeed, rightSpeed);
    const float dtSec = static_cast<float>(elapsed) / 1000.0f;
    OdometerState::g_heading = updateHeading(
        OdometerState::g_heading, angularVel, dtSec
    );
    
    // 更新时间戳
    OdometerState::g_lastSampleTime = now;
}

/**
 * 获取当前测速数据
 * 返回不可变的测速数据结构
 * 
 * 注意：读取 volatile 脉冲计数器时必须关中断，
 * 防止ISR在读取过程中修改数据导致撕裂读取
 */
inline OdometryData getCurrentOdometry() {
    // 关中断读取所有 ISR 共享的 volatile 变量和由主循环更新的浮点变量
    // 防止 ISR 在读取过程中修改脉冲计数器导致数据不一致
    noInterrupts();
    const uint32_t leftPulses = OdometerState::g_leftPulses;
    const uint32_t rightPulses = OdometerState::g_rightPulses;
    
    // 读取由 updateOdometer() 更新的浮点变量（主循环和此函数可能并发访问）
    const float leftRpm = OdometerState::g_leftRpm;
    const float leftSpeedMmps = OdometerState::g_leftSpeedMmps;
    const float leftDistanceMm = OdometerState::g_leftDistanceMm;
    const float rightRpm = OdometerState::g_rightRpm;
    const float rightSpeedMmps = OdometerState::g_rightSpeedMmps;
    const float rightDistanceMm = OdometerState::g_rightDistanceMm;
    const float heading = OdometerState::g_heading;
    const float totalDistanceMm = OdometerState::g_totalDistanceMm;
    const uint32_t lastSampleTime = OdometerState::g_lastSampleTime;
    interrupts();
    
    return OdometryData(
        WheelSpeed(
            leftRpm,
            leftSpeedMmps,
            leftDistanceMm,
            leftPulses,
            lastSampleTime
        ),
        WheelSpeed(
            rightRpm,
            rightSpeedMmps,
            rightDistanceMm,
            rightPulses,
            lastSampleTime
        ),
        (leftSpeedMmps + rightSpeedMmps) / 2.0f,
        calculateAngularVelocity(leftSpeedMmps, rightSpeedMmps),
        heading,
        totalDistanceMm,
        lastSampleTime
    );
}

// ============================================
// 校准函数
// ============================================

/**
 * 设置校准参数
 * 用于补偿左右轮速度差异
 * 输入：左轮修正系数，右轮修正系数
 */
inline void setSpeedCalibration(float leftCorrection, float rightCorrection) {
    OdometerState::g_calibration = SpeedCalibration(leftCorrection, rightCorrection);
    Serial.printf("[测速模块] 校准参数更新: 左=%.3f, 右=%.3f\n", 
                  leftCorrection, rightCorrection);
}

/**
 * 自动校准：直行时记录速度比，计算修正系数
 * 调用条件：车在平地上直行一段距离后调用
 * 输入：当前PWM值，期望直行距离
 * 输出：校准参数
 */
inline SpeedCalibration autoCalibrate() {
    // 如果左右轮速度都接近0，返回默认校准
    if (OdometerState::g_leftSpeedMmps < 1.0f || OdometerState::g_rightSpeedMmps < 1.0f) {
        Serial.println("[测速模块] 自动校准失败：速度过低");
        return OdometerConfig::DEFAULT_CALIBRATION;
    }
    
    // 计算速比（以较慢轮为基准）
    const float leftSpeed = OdometerState::g_leftSpeedMmps;
    const float rightSpeed = OdometerState::g_rightSpeedMmps;
    const float avgSpeed = (leftSpeed + rightSpeed) / 2.0f;
    
    // 修正系数：使轮速趋向平均值
    const float leftCorrection = (avgSpeed > 0.1f) ? avgSpeed / leftSpeed : 1.0f;
    const float rightCorrection = (avgSpeed > 0.1f) ? avgSpeed / rightSpeed : 1.0f;
    
    // 修正系数上限约束，防止极端值导致 PID 振荡
    constexpr float MIN_CORRECTION = 0.5f;
    constexpr float MAX_CORRECTION = 2.0f;
    const float clampedLeft = (leftCorrection < MIN_CORRECTION) ? MIN_CORRECTION 
                            : (leftCorrection > MAX_CORRECTION) ? MAX_CORRECTION 
                            : leftCorrection;
    const float clampedRight = (rightCorrection < MIN_CORRECTION) ? MIN_CORRECTION 
                             : (rightCorrection > MAX_CORRECTION) ? MAX_CORRECTION 
                             : rightCorrection;
    
    Serial.printf("[测速模块] 自动校准结果: 左=%.3f, 右=%.3f\n",
                  clampedLeft, clampedRight);
    
    return SpeedCalibration(clampedLeft, clampedRight);
}

#endif // ODOMETER_H