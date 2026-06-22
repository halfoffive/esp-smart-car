/**
 * 测速模块 - 函数式编程风格
 * 基于 ESP32-S3（Freenove FNK0085），使用光电编码器（光栅码盘 + 光敏元件）测量轮速
 * 
 * 功能：
 * 1. 中断方式读取编码器脉冲
 * 2. 计算左右轮实时速度 (RPM)
 * 3. 计算行走距离和加速度
 * 4. 提供测速数据供PID控制器使用
 * 
 * 硬件：
 * - 左轮编码器: GPIO 1 (中断引脚，S3 安全 GPIO)
 * - 右轮编码器: GPIO 2 (中断引脚，S3 安全 GPIO)
 * - 编码器: 光电编码器（光栅码盘 + 光敏元件），每圈20个脉冲
 * 
 * 作者：智能车项目团队
 * 版本：1.4.0（修复 P2-02 编码器方向、P2-05 自动校准直行判断）
 * 日期：2026-06-20
 */

#ifndef ODOMETER_H
#define ODOMETER_H

#include <Arduino.h>
#include "motor_control.h"  // 使用 MotorDirection 方向枚举

// ============================================
// 纯数据类型定义
// ============================================

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
    // 编码器引脚（ESP32-S3 GPIO 1 和 GPIO 2 支持中断，避开摄像头占用的 GPIO 4-18）
    constexpr uint8_t LEFT_ENCODER_PIN = 1;
    constexpr uint8_t RIGHT_ENCODER_PIN = 2;
    
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
// 声明为 extern，在 car_controller.ino 中做唯一定义，避免头文件被多次包含时重复定义
// ============================================
namespace OdometerState {
    // 脉冲计数（volatile 用于中断安全）
    extern volatile uint32_t g_leftPulses;
    extern volatile uint32_t g_rightPulses;
    extern uint32_t g_lastLeftPulses;    // 非 ISR 变量，仅主循环访问
    extern uint32_t g_lastRightPulses;   // 非 ISR 变量，仅主循环访问

    // 累计距离
    extern float g_leftDistanceMm;
    extern float g_rightDistanceMm;

    // 速度计算
    extern float g_leftSpeedMmps;
    extern float g_rightSpeedMmps;
    extern float g_leftRpm;
    extern float g_rightRpm;

    // 航向
    extern float g_heading;
    extern float g_totalDistanceMm;

    // 时间
    extern uint32_t g_lastSampleTime;

    // 校准
    extern SpeedCalibration g_calibration;
}

// ============================================
// 中断服务函数
// ============================================

/**
 * 左轮编码器中断
 * 每检测到一个脉冲递增计数
 *
 * 重要：ISR 函数体定义在 car_controller.ino 中（第 109-115 行附近），
 * 且带有 IRAM_ATTR 属性。请勿在头文件中用 inline 实现这些 ISR，
 * 否则 inline + IRAM_ATTR 组合可能触发 literal pool 重定位错误
 * （编译器将常量放入 Flash，而 IRAM 中断代码无法访问 Flash 上的 literal pool）。
 */
void IRAM_ATTR onLeftEncoderPulse();

/**
 * 右轮编码器中断
 * 每检测到一个脉冲递增计数
 *
 * 重要：ISR 函数体定义在 car_controller.ino 中（第 109-115 行附近），
 * 且带有 IRAM_ATTR 属性。请勿在头文件中用 inline 实现这些 ISR，
 * 否则 inline + IRAM_ATTR 组合可能触发 literal pool 重定位错误。
 */
void IRAM_ATTR onRightEncoderPulse();

// ============================================
// 初始化函数
// ============================================

/**
 * 初始化编码器引脚和中断
 * 副作用：配置GPIO和中断
 *
 * 注意：本函数不输出串口日志，因为可能在 Serial.begin 完成前被调用。
 *       调用方（car_controller.ino 的 setup()）负责在 Serial 就绪后打印初始化信息。
 */
inline void initializeOdometer() {
    pinMode(OdometerConfig::LEFT_ENCODER_PIN, INPUT_PULLUP);
    pinMode(OdometerConfig::RIGHT_ENCODER_PIN, INPUT_PULLUP);

    // 在 attach 中断前清零所有状态（ISR 仅做原子自增，即使立即触发也无数据破坏风险）
    // 注意：不再使用 noInterrupts()/interrupts()——ESP32-S3 上 attachInterrupt 内部
    // 配置 GPIO 中断矩阵可能耗时较长（>300ms），在关中断下会导致 IWDT 超时 panic
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

    attachInterrupt(digitalPinToInterrupt(OdometerConfig::LEFT_ENCODER_PIN),
                    onLeftEncoderPulse, FALLING);
    attachInterrupt(digitalPinToInterrupt(OdometerConfig::RIGHT_ENCODER_PIN),
                    onRightEncoderPulse, FALLING);

    Serial.println("[测速模块] 初始化完成（编码器中断已挂载）");
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
    // 更新航向角并归一化到 [0, 2π)，防止浮点数值无界增长
    float newHeading = heading + angularVelocity * dtSec;
    // 归一化到 [0, 2π)
    // TWO_PI 为 Arduino.h 预定义宏 (6.283185...)
    newHeading = fmod(newHeading, static_cast<float>(TWO_PI));
    if (newHeading < 0.0f) {
        newHeading += static_cast<float>(TWO_PI);
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
 *
 * ISR 安全说明：本函数使用 noInterrupts()/interrupts() 保护 volatile 脉冲计数器的读取。
 * ESP32 上 32-bit float 读写为硬件原子操作，因此 g_leftSpeedMmps/g_rightSpeedMmps/g_heading
 * 的浮点更新无需额外同步。调用者（loop()）运行在主任务上下文，本函数仅在 loop() 中执行写入，风险可控。
 *
 * 方向符号：FORWARD=+1，BACKWARD=-1，STOP=0。脉冲差与距离/速度均乘以方向符号，
 * 使后退时里程和航向积分与运动方向一致。
 */
inline void updateOdometer(MotorDirection leftDir, MotorDirection rightDir) {
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

    // 根据电机方向确定脉冲符号
    const int8_t leftSign = (leftDir == MotorDirection::FORWARD) ? 1
                          : (leftDir == MotorDirection::BACKWARD) ? -1 : 0;
    const int8_t rightSign = (rightDir == MotorDirection::FORWARD) ? 1
                           : (rightDir == MotorDirection::BACKWARD) ? -1 : 0;

    // 计算速度（应用校准系数与方向符号）
    const float leftSpeed = calculateSpeedMmps(leftDelta, elapsed)
                          * leftSign
                          * OdometerState::g_calibration.leftCorrection;
    const float rightSpeed = calculateSpeedMmps(rightDelta, elapsed)
                           * rightSign
                           * OdometerState::g_calibration.rightCorrection;

    OdometerState::g_leftSpeedMmps = leftSpeed;
    OdometerState::g_rightSpeedMmps = rightSpeed;
    OdometerState::g_leftRpm = calculateRpm(leftDelta, elapsed)
                               * leftSign
                               * OdometerState::g_calibration.leftCorrection;
    OdometerState::g_rightRpm = calculateRpm(rightDelta, elapsed)
                                * rightSign
                                * OdometerState::g_calibration.rightCorrection;

    // 累计距离（带方向符号）
    const float leftDistDelta = static_cast<float>(leftDelta) * OdometerConfig::MM_PER_PULSE * leftSign;
    const float rightDistDelta = static_cast<float>(rightDelta) * OdometerConfig::MM_PER_PULSE * rightSign;
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
 * 调用条件：车在平地上同向直行一段距离后调用
 * 输入：左电机方向，右电机方向
 * 输出：校准参数
 */
inline SpeedCalibration autoCalibrate(MotorDirection leftDir, MotorDirection rightDir) {
    constexpr float MIN_SPEED_THRESHOLD = 1.0f;

    // 必须同向直行（同时前进或同时后退），转弯/停止时不允许校准
    const bool isStraight = ((leftDir == MotorDirection::FORWARD && rightDir == MotorDirection::FORWARD) ||
                             (leftDir == MotorDirection::BACKWARD && rightDir == MotorDirection::BACKWARD));
    if (!isStraight ||
        fabs(OdometerState::g_leftSpeedMmps) < MIN_SPEED_THRESHOLD ||
        fabs(OdometerState::g_rightSpeedMmps) < MIN_SPEED_THRESHOLD) {
        Serial.println("[测速模块] 自动校准失败：未直行或速度过低");
        return OdometerConfig::DEFAULT_CALIBRATION;
    }

    // 计算速比（使用绝对值，避免同向后退时符号影响）
    const float leftSpeed = fabs(OdometerState::g_leftSpeedMmps);
    const float rightSpeed = fabs(OdometerState::g_rightSpeedMmps);
    const float avgSpeed = (leftSpeed + rightSpeed) / 2.0f;

    // 修正系数：使轮速趋向平均值（同时检查除数为零）
    const float leftCorrection = (avgSpeed > 0.1f && leftSpeed > 0.1f) ? avgSpeed / leftSpeed : 1.0f;
    const float rightCorrection = (avgSpeed > 0.1f && rightSpeed > 0.1f) ? avgSpeed / rightSpeed : 1.0f;

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