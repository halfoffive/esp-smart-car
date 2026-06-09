/**
 * PID 控制器 - 函数式编程风格
 * 基于 ESP32-C6，用于智能车直线行走修正
 * 
 * 功能：
 * 1. PID 算法实现（位置式PID）
 * 2. 左右轮速度差补偿
 * 3. 直线行走模式 - 自动修正偏航
 * 4. 精确方向控制
 * 
 * 核心思想：
 * - 通过编码器测速获取左右轮实际速度
 * - PID 计算修正量补偿电机速度差异
 * - 前进/后退时自动保持直线
 * - 转弯时使用差速控制精确方向
 * 
 * 作者：智能车项目团队
 * 版本：1.2.0
 */

#ifndef PID_CONTROL_H
#define PID_CONTROL_H

#include <Arduino.h>
#include "odometer.h"

// ============================================
// 纯数据类型定义
// ============================================

/**
 * PID 参数
 * 不可变参数结构体，用于配置PID控制器
 */
struct PIDParams {
    const float kp;            // 比例系数
    const float ki;            // 积分系数
    const float kd;            // 微分系数
    const float outputMin;     // 输出最小值
    const float outputMax;     // 输出最大值
    const float integralLimit; // 积分限幅（防积分饱和）
    
    constexpr PIDParams(
        float p, float i, float d,
        float minOut, float maxOut, float iLimit
    ) : kp(p), ki(i), kd(d),
        outputMin(minOut), outputMax(maxOut), integralLimit(iLimit) {}
};

/**
 * PID 控制器状态
 * 记录PID计算的中间状态
 */
struct PIDState {
    const float setpoint;          // 目标值
    const float input;             // 输入值（当前测量值）
    const float output;            // 输出值（修正量）
    const float error;             // 当前误差
    const float lastError;         // 上次误差
    const float integral;          // 积分累计
    const float derivative;        // 微分项
    const uint32_t lastTime;       // 上次更新时间
    
    constexpr PIDState(
        float sp, float inp, float out, float err, float lastErr,
        float integ, float deriv, uint32_t lt
    ) : setpoint(sp), input(inp), output(out), error(err),
        lastError(lastErr), integral(integ), derivative(deriv), lastTime(lt) {}
};

/**
 * 智能控制输出
 * 包含左右电机的修正后PWM值和方向
 */
struct SmartMotorOutput {
    const uint8_t leftPwm;          // 左电机PWM（0-255）
    const uint8_t rightPwm;         // 右电机PWM（0-255）
    const MotorDirection leftDir;    // 左电机方向
    const MotorDirection rightDir;  // 右电机方向
    const float correction;         // PID修正量
    const bool isStraightLine;     // 是否在直线模式
    
    constexpr SmartMotorOutput(
        uint8_t lp, uint8_t rp, MotorDirection ld, MotorDirection rd,
        float corr, bool straight
    ) : leftPwm(lp), rightPwm(rp), leftDir(ld), rightDir(rd),
        correction(corr), isStraightLine(straight) {}
};

/**
 * 智能行走模式
 */
enum class DriveMode : uint8_t {
    NORMAL = 0,         // 普通模式（无修正）
    STRAIGHT_LINE = 1,  // 直线修正模式
    HEADING_LOCK = 2    // 锁定航向模式
};

// ============================================
// 默认PID参数
// ============================================
namespace PIDDefaults {
    // 直线修正PID参数（经验值，可能需要根据实际调试）
    // kp: 对速度差响应较快
    // ki: 消除稳态误差
    // kd: 抑制超调
    constexpr PIDParams STRAIGHT_PID = PIDParams(
        0.8f,    // kp - 比例系数
        0.05f,   // ki - 积分系数（较小，防止超调）
        0.3f,    // kd - 微分系数
        -80.0f,  // 输出最小值（最大反向修正PWM）
        80.0f,   // 输出最大值（最大正向修正PWM）
        50.0f    // 积分限幅
    );
    
    // 航向锁定PID参数
    constexpr PIDParams HEADING_PID = PIDParams(
        1.2f,    // kp - 比例系数（较大，快速纠正偏航）
        0.02f,   // ki - 积分系数
        0.5f,    // kd - 微分系数
        -100.0f, // 输出最小值
        100.0f,  // 输出最大值
        30.0f    // 积分限幅
    );
}

// ============================================
// 全局状态
// ============================================
namespace PIDControllerState {
    // PID 状态
    PIDState g_straightPidState = PIDState(0, 0, 0, 0, 0, 0, 0, 0);
    PIDState g_headingPidState = PIDState(0, 0, 0, 0, 0, 0, 0, 0);
    
    // 行走模式
    DriveMode g_driveMode = DriveMode::STRAIGHT_LINE;
    
    // 目标航向（锁定航向模式下使用）
    float g_targetHeading = 0.0f;
    
    // 直线模式使能
    bool g_straightLineEnabled = true;
}

// ============================================
// 纯函数：PID 计算
// ============================================

/**
 * 纯函数：PID 计算核心
 * 标准位置式PID算法
 * 输入：PID参数，当前状态，新输入值，目标值
 * 输出：新PID状态
 */
inline PIDState computePID(
    const PIDParams& params,
    const PIDState& lastState,
    float newInput,
    float setpoint,
    uint32_t currentTime
) {
    // 计算时间间隔（秒）
    // 溢出安全性说明：millis() 返回 uint32_t，约 49 天溢出回绕。
    // 无符号减法 (currentTime - lastState.lastTime) 在 C++ 中
    // 对 uint32_t 类型是安全的：即使溢出，差值仍然正确（模 2^32 算术）。
    // 此处先计算无符号差值，再转换为 float，确保溢出安全。
    const uint32_t dtMs = currentTime - lastState.lastTime;
    const float dt = (dtMs > 0) 
        ? static_cast<float>(dtMs) / 1000.0f 
        : 0.01f;
    
    // 当前误差
    const float error = setpoint - newInput;
    
    // 比例项
    const float proportional = params.kp * error;
    
    // 积分项（带限幅防饱和）
    float newIntegral = lastState.integral + error * dt;
    if (newIntegral > params.integralLimit) {
        newIntegral = params.integralLimit;
    } else if (newIntegral < -params.integralLimit) {
        newIntegral = -params.integralLimit;
    }
    const float integralTerm = params.ki * newIntegral;
    
    // 微分项
    const float derivativeTerm = params.kd * (error - lastState.error) / dt;
    
    // 计算输出
    float output = proportional + integralTerm + derivativeTerm;
    
    // 输出限幅
    if (output > params.outputMax) output = params.outputMax;
    if (output < params.outputMin) output = params.outputMin;
    
    return PIDState(
        setpoint, newInput, output, error,
        error, newIntegral, derivativeTerm, currentTime
    );
}

// ============================================
// 智能控制：直线修正
// ============================================

/**
 * 纯函数：应用直线修正到电机PWM
 * 当车在前进/后退时，根据左右轮速度差自动修正
 * 
 * 输入：
 *   - basePwm: 基础PWM值
 *   - leftDir: 左电机方向
 *   - rightDir: 右电机方向
 *   - leftSpeedMmps: 左轮实际速度(mm/s)
 *   - rightSpeedMmps: 右轮实际速度(mm/s)
 *   - correction: PID修正量
 * 
 * 输出：修正后的电机输出
 */
inline SmartMotorOutput applyStraightCorrection(
    uint8_t basePwm,
    MotorDirection leftDir,
    MotorDirection rightDir,
    float leftSpeedMmps,
    float rightSpeedMmps,
    float correction
) {
    // 修正量应用到左右轮PWM
    // 左轮减去修正量，右轮加上修正量
    // 这样如果右轮快了，修正量为正，左轮加速右轮减速
    int leftPwm = static_cast<int>(basePwm) - static_cast<int>(correction);
    int rightPwm = static_cast<int>(basePwm) + static_cast<int>(correction);
    
    // 限幅
    if (leftPwm < 0) leftPwm = 0;
    if (leftPwm > 255) leftPwm = 255;
    if (rightPwm < 0) rightPwm = 0;
    if (rightPwm > 255) rightPwm = 255;
    
    return SmartMotorOutput(
        static_cast<uint8_t>(leftPwm),
        static_cast<uint8_t>(rightPwm),
        leftDir,
        rightDir,
        correction,
        true
    );
}

/**
 * 初始化PID控制器
 */
inline void initializePIDController() {
    PIDControllerState::g_straightPidState = PIDState(0, 0, 0, 0, 0, 0, 0, millis());
    PIDControllerState::g_headingPidState = PIDState(0, 0, 0, 0, 0, 0, 0, millis());
    PIDControllerState::g_driveMode = DriveMode::STRAIGHT_LINE;
    PIDControllerState::g_targetHeading = 0.0f;
    PIDControllerState::g_straightLineEnabled = true;
    
    Serial.println("[PID控制器] 初始化完成");
    Serial.printf("  直线PID: Kp=%.2f, Ki=%.3f, Kd=%.2f\n",
                  PIDDefaults::STRAIGHT_PID.kp,
                  PIDDefaults::STRAIGHT_PID.ki,
                  PIDDefaults::STRAIGHT_PID.kd);
}

/**
 * 更新直线修正PID
 * 在每个控制周期调用
 * 返回修正后的电机输出
 */
inline SmartMotorOutput updateSmartControl(
    uint8_t basePwm,
    MotorDirection leftDir,
    MotorDirection rightDir
) {
    const uint32_t now = millis();
    
    // 如果直线模式未启用或电机停止，直接输出基础PWM
    if (!PIDControllerState::g_straightLineEnabled || 
        leftDir == MotorDirection::STOP) {
        return SmartMotorOutput(
            basePwm, basePwm, leftDir, rightDir, 0.0f, false
        );
    }
    
    // 获取当前速度数据
    const float leftSpeed = OdometerState::g_leftSpeedMmps;
    const float rightSpeed = OdometerState::g_rightSpeedMmps;
    
    // 计算速度差（目标：左右轮速度相等，即差值为0）
    const float speedDiff = rightSpeed - leftSpeed;
    
    // PID 计算：目标是速度差为0
    PIDControllerState::g_straightPidState = computePID(
        PIDDefaults::STRAIGHT_PID,
        PIDControllerState::g_straightPidState,
        speedDiff,    // 输入：速度差
        0.0f,         // 目标值：速度差为0
        now
    );
    
    // 应用修正
    const float correction = PIDControllerState::g_straightPidState.output;
    
    return applyStraightCorrection(
        basePwm, leftDir, rightDir,
        leftSpeed, rightSpeed, correction
    );
}

/**
 * 切换行走模式
 */
inline void setDriveMode(DriveMode mode) {
    PIDControllerState::g_driveMode = mode;
    
    // 切换到锁定航向模式时，锁定当前航向
    if (mode == DriveMode::HEADING_LOCK) {
        PIDControllerState::g_targetHeading = OdometerState::g_heading;
    }
    
    const char* modeName = "";
    switch (mode) {
        case DriveMode::NORMAL: modeName = "普通模式"; break;
        case DriveMode::STRAIGHT_LINE: modeName = "直线修正"; break;
        case DriveMode::HEADING_LOCK: modeName = "航向锁定"; break;
    }
    Serial.printf("[PID控制器] 行走模式切换: %s\n", modeName);
}

/**
 * 启用/禁用直线修正
 */
inline void setStraightLineEnabled(bool enabled) {
    PIDControllerState::g_straightLineEnabled = enabled;
    Serial.printf("[PID控制器] 直线修正: %s\n", enabled ? "启用" : "禁用");
    
    // 重置PID状态
    if (enabled) {
        PIDControllerState::g_straightPidState = PIDState(0, 0, 0, 0, 0, 0, 0, millis());
    }
}

/**
 * 获取当前行走模式
 */
inline DriveMode getCurrentDriveMode() {
    return PIDControllerState::g_driveMode;
}

/**
 * 获取直线修正使能状态
 */
inline bool isStraightLineEnabled() {
    return PIDControllerState::g_straightLineEnabled;
}

#endif // PID_CONTROL_H