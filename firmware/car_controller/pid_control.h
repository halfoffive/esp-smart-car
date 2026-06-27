/**
 * PID 控制器 - 函数式编程风格
 * 基于 ESP32-S3，用于智能车直线行走修正
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
 * 版本：1.4.0（FW-H2 lastInput初始化、FW-M3 clamping抗饱和）
 * 日期：2026-06-20
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
    float kp;            // 比例系数
    float ki;            // 积分系数
    float kd;            // 微分系数
    float outputMin;     // 输出最小值
    float outputMax;     // 输出最大值
    float integralLimit; // 积分限幅（防积分饱和）
    
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
    float setpoint;          // 目标值
    float input;             // 输入值（当前测量值）
    float output;            // 输出值（修正量）
    float error;             // 当前误差
    float integral;          // 积分累计
    float derivative;        // 微分项
    uint32_t lastTime;       // 上次更新时间

    constexpr PIDState(
        float sp, float inp, float out, float err,
        float integ, float deriv, uint32_t lt
    ) : setpoint(sp), input(inp), output(out), error(err),
        integral(integ), derivative(deriv), lastTime(lt) {}
};

/**
 * 智能控制输出
 * 包含左右电机的修正后PWM值和方向
 */
struct SmartMotorOutput {
    uint8_t leftPwm;          // 左电机PWM（0-255）
    uint8_t rightPwm;         // 右电机PWM（0-255）
    MotorDirection leftDir;    // 左电机方向
    MotorDirection rightDir;  // 右电机方向
    float correction;         // PID修正量
    bool isStraightLine;     // 是否在直线模式
    
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
// 声明为 extern，在 car_controller.ino 中做唯一定义
// ============================================
namespace PIDControllerState {
    // PID 状态
    extern PIDState g_straightPidState;
    extern PIDState g_headingPidState;

    // 行走模式（默认普通模式，与 car_controller.ino 中 g_smartDriveEnabled=false 一致）
    extern DriveMode g_driveMode;

    // 航向锁定目标角度（进入锁定模式时捕获当前航向）
    extern float g_headingLockTarget;
    // 航向锁定目标是否已初始化
    extern bool g_headingLockTargetInitialized;
}

// ============================================
// 纯函数：PID 计算
// ============================================

/**
 * 纯函数：PID 计算核心
 * 标准位置式PID算法
 * 输入：PID参数，当前状态，新输入值，目标值
 * 输出：新PID状态
 *
 * 实现要点：
 * - 微分项使用 -d(input)/dt，避免 setpoint 跳变导致的 derivative kick。
 * - FW-M3: 积分抗饱和使用 clamping 方法：当输出饱和且误差使饱和加剧时，不累积积分。
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

    // dtMs 为 0 表示时间未推进，直接返回上次状态，避免除零
    if (dtMs == 0) {
        return lastState;
    }

    const float dt = static_cast<float>(dtMs) / 1000.0f;

    // 当前误差
    const float error = setpoint - newInput;

    // 比例项
    const float proportional = params.kp * error;

    // 微分项：对测量值微分，避免 setpoint 跳变产生 derivative kick
    const float derivativeTerm = params.kd * -(newInput - lastState.input) / dt;

    // FW-M3: Clamping 抗饱和（标准实现）
    // 1. 先计算新积分项
    float newIntegral = lastState.integral + error * dt;
    // 积分限幅（硬限保护）
    if (newIntegral > params.integralLimit) {
        newIntegral = params.integralLimit;
    } else if (newIntegral < -params.integralLimit) {
        newIntegral = -params.integralLimit;
    }
    // 2. 计算包含新积分的完整输出
    const float integralTerm = params.ki * newIntegral;
    float output = proportional + integralTerm + derivativeTerm;
    // 3. 判断是否饱和
    const bool saturatedHigh = (output >= params.outputMax);
    const bool saturatedLow = (output <= params.outputMin);
    // 4. 如果输出饱和且误差方向使饱和加剧，则回退积分（clamping）
    if ((saturatedHigh && error > 0.0f) || (saturatedLow && error < 0.0f)) {
        newIntegral = lastState.integral;
        output = proportional + params.ki * newIntegral + derivativeTerm;
    }
    // 5. 最终输出限幅
    if (output > params.outputMax) output = params.outputMax;
    if (output < params.outputMin) output = params.outputMin;

    return PIDState(
        setpoint, newInput, output, error,
        newIntegral, derivativeTerm, currentTime
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
    // 后退时左右轮转向效果与前进相反，因此交换修正量符号
    const bool isReversing = (leftDir == MotorDirection::BACKWARD && rightDir == MotorDirection::BACKWARD);
    const float signedCorrection = isReversing ? -correction : correction;
    int leftPwm = static_cast<int>(basePwm) - static_cast<int>(signedCorrection);
    int rightPwm = static_cast<int>(basePwm) + static_cast<int>(signedCorrection);

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
 * 默认状态与 car_controller.ino 中 g_smartDriveEnabled = false 一致
 */
inline void initializePIDController() {
    const uint32_t now = millis();
    // FW-H2: 初始化时lastInput为0（尚无测量数据）
    PIDControllerState::g_straightPidState = PIDState(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, now);
    PIDControllerState::g_headingPidState = PIDState(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, now);
    PIDControllerState::g_driveMode = DriveMode::NORMAL;

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
 *
 * 调用上下文要求：本函数读取 OdometerState 中的浮点速度/航向变量。
 * 这些变量仅由 loop() 中的 updateOdometer() 写入，而本函数也在 loop() 中调用，
 * 主循环为单任务顺序执行，不存在并发写入。ESP32 上 32-bit float 读写为硬件原子操作，
 * 因此无需额外 noInterrupts()/interrupts() 保护（ISR 只修改 volatile 脉冲计数器，
 * 不修改这些浮点变量）。
 */
inline SmartMotorOutput updateSmartControl(
    uint8_t basePwm,
    MotorDirection leftDir,
    MotorDirection rightDir
) {
    const uint32_t now = millis();

    // 如果电机停止，直接输出基础PWM
    if (leftDir == MotorDirection::STOP) {
        return SmartMotorOutput(
            basePwm, basePwm, leftDir, rightDir, 0.0f, false
        );
    }

    const DriveMode currentMode = PIDControllerState::g_driveMode;

    if (currentMode == DriveMode::NORMAL) {
        // 普通模式：无修正
        return SmartMotorOutput(
            basePwm, basePwm, leftDir, rightDir, 0.0f, false
        );
    }

    // 获取当前速度数据
    const float leftSpeed = OdometerState::g_leftSpeedMmps;
    const float rightSpeed = OdometerState::g_rightSpeedMmps;

    if (currentMode == DriveMode::STRAIGHT_LINE) {
        // 直线修正模式：使用速度差 PID
        const float speedDiff = rightSpeed - leftSpeed;
        PIDControllerState::g_straightPidState = computePID(
            PIDDefaults::STRAIGHT_PID,
            PIDControllerState::g_straightPidState,
            speedDiff,
            0.0f,
            now
        );
        const float correction = PIDControllerState::g_straightPidState.output;
        return applyStraightCorrection(
            basePwm, leftDir, rightDir,
            leftSpeed, rightSpeed, correction
        );
    }

    if (currentMode == DriveMode::HEADING_LOCK) {
        // 航向锁定模式：使用航向 PID，锁定当前航向角
        // 目标航向为进入锁定模式时的航向
        if (!PIDControllerState::g_headingLockTargetInitialized) {
            PIDControllerState::g_headingLockTarget = OdometerState::g_heading;
            PIDControllerState::g_headingLockTargetInitialized = true;
        }

        // 显式计算航向误差：当前航向 - 目标航向，并归一化到 [-PI, PI]
        // 防止角度跨 0/2PI 边界时误差跳变
        // 传给 computePID 的 input 为该误差，setpoint=0，因此内部 error = -(heading - target)
        float headingError = OdometerState::g_heading - PIDControllerState::g_headingLockTarget;
        if (headingError > M_PI) headingError -= 2.0f * M_PI;
        if (headingError < -M_PI) headingError += 2.0f * M_PI;
        PIDControllerState::g_headingPidState = computePID(
            PIDDefaults::HEADING_PID,
            PIDControllerState::g_headingPidState,
            headingError,
            0.0f,
            now
        );
        const float correction = PIDControllerState::g_headingPidState.output;
        return applyStraightCorrection(
            basePwm, leftDir, rightDir,
            leftSpeed, rightSpeed, correction
        );
    }

    // 默认：无修正
    return SmartMotorOutput(
        basePwm, basePwm, leftDir, rightDir, 0.0f, false
    );
}

/**
 * 切换行走模式
 * FW-H2: PID重置时lastInput（input字段）初始化为当前实际测量值，避免微分冲击
 */
inline void setDriveMode(DriveMode mode) {
    const uint32_t now = millis();

    // 进入或退出航向锁定模式时，统一重置锁定目标与航向 PID 状态
    const bool enteringHeadingLock = (mode == DriveMode::HEADING_LOCK);
    const bool exitingHeadingLock = (PIDControllerState::g_driveMode == DriveMode::HEADING_LOCK &&
                                     mode != DriveMode::HEADING_LOCK);
    if (enteringHeadingLock || exitingHeadingLock) {
        PIDControllerState::g_headingLockTargetInitialized = false;
        if (enteringHeadingLock) {
            // FW-H2: 航向PID初始化时input设为0（刚锁定目标，航向误差为0）
            PIDControllerState::g_headingPidState = PIDState(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, now);
        }
    }

    // 切换到直线修正模式时，重置直线 PID 状态
    if (mode == DriveMode::STRAIGHT_LINE) {
        // FW-H2: 直线PID初始化时input设为当前速度差（lastInput）
        const float currentSpeedDiff = OdometerState::g_rightSpeedMmps - OdometerState::g_leftSpeedMmps;
        PIDControllerState::g_straightPidState = PIDState(0.0f, currentSpeedDiff, 0.0f, 0.0f, 0.0f, 0.0f, now);
    }

    PIDControllerState::g_driveMode = mode;

    const char* modeName = "";
    switch (mode) {
        case DriveMode::NORMAL: modeName = "普通模式"; break;
        case DriveMode::STRAIGHT_LINE: modeName = "直线修正"; break;
        case DriveMode::HEADING_LOCK: modeName = "航向锁定"; break;
    }
    Serial.printf("[PID控制器] 行走模式切换: %s\n", modeName);
}

/**
 * 获取当前行走模式
 */
inline DriveMode getCurrentDriveMode() {
    return PIDControllerState::g_driveMode;
}

#endif // PID_CONTROL_H