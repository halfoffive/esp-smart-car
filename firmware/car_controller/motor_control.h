/**
 * 智能车控制系统 - 函数式编程风格
 * 基于 ESP32-C6，使用 L298N 驱动 4 个电机，SG90 舵机控制转向
 * 作者：智能车项目团队
 * 版本：1.2.0
 */

#ifndef MOTOR_CONTROL_H
#define MOTOR_CONTROL_H

#include <Arduino.h>

// ============================================
// 纯数据类型定义
// ============================================

/**
 * 电机方向枚举
 * 使用函数式风格：数据不可变，状态显式传递
 */
enum class MotorDirection : uint8_t {
    STOP = 0,      // 停止
    FORWARD = 1,   // 正转
    BACKWARD = 2   // 反转
};

/**
 * 单个电机状态结构体
 * 所有字段均为 const，确保不可变性
 */
struct MotorState {
    uint8_t pinIn1;        // 输入引脚1
    uint8_t pinIn2;        // 输入引脚2
    uint8_t pinEn;         // 使能引脚(PWM)
    MotorDirection direction; // 当前方向
    uint8_t speed;         // 当前速度 (0-255)
    
    // 构造函数 - 显式初始化所有字段
    constexpr MotorState(
        uint8_t in1, uint8_t in2, uint8_t en,
        MotorDirection dir, uint8_t spd
    ) : pinIn1(in1), pinIn2(in2), pinEn(en),
        direction(dir), speed(spd) {}
};

/**
 * 整车运动状态
 * 包含4个电机的独立状态
 */
struct VehicleMotion {
    MotorState frontLeft;   // 左前轮
    MotorState frontRight;  // 右前轮
    MotorState rearLeft;    // 左后轮
    MotorState rearRight;   // 右后轮
    
    constexpr VehicleMotion(
        MotorState fl, MotorState fr,
        MotorState rl, MotorState rr
    ) : frontLeft(fl), frontRight(fr),
        rearLeft(rl), rearRight(rr) {}
};

// ============================================
// 引脚配置常量
// ============================================
namespace PinConfig {
    // L298N 模块1（控制左侧两个电机）
    constexpr uint8_t L298N_1_IN1 = 4;   // 左侧输入1
    constexpr uint8_t L298N_1_IN2 = 5;   // 左侧输入2
    constexpr uint8_t L298N_1_EN = 6;    // 左侧使能(PWM)
    
    // L298N 模块2（控制右侧两个电机）
    constexpr uint8_t L298N_2_IN1 = 7;   // 右侧输入1
    constexpr uint8_t L298N_2_IN2 = 8;   // 右侧输入2
    constexpr uint8_t L298N_2_EN = 9;    // 右侧使能(PWM)

    // 别名：左侧电机使用 L298N 模块1
    constexpr uint8_t MOTOR_LEFT_IN1 = L298N_1_IN1;   // 左侧输入1 (GPIO 4)
    constexpr uint8_t MOTOR_LEFT_IN2 = L298N_1_IN2;   // 左侧输入2 (GPIO 5)
    // 别名：右侧电机使用 L298N 模块2
    constexpr uint8_t MOTOR_RIGHT_IN1 = L298N_2_IN1;  // 右侧输入1 (GPIO 7)
    constexpr uint8_t MOTOR_RIGHT_IN2 = L298N_2_IN2;  // 右侧输入2 (GPIO 8)
}

// ============================================
// 纯函数：电机控制逻辑
// ============================================

/**
 * 初始化电机引脚
 * 副作用函数：执行硬件初始化
 * 返回：void
 */
inline void initializeMotorPins() {
    // 配置所有引脚为输出模式
    pinMode(PinConfig::L298N_1_IN1, OUTPUT);
    pinMode(PinConfig::L298N_1_IN2, OUTPUT);
    pinMode(PinConfig::L298N_1_EN, OUTPUT);
    pinMode(PinConfig::L298N_2_IN1, OUTPUT);
    pinMode(PinConfig::L298N_2_IN2, OUTPUT);
    pinMode(PinConfig::L298N_2_EN, OUTPUT);
    
    // 初始状态：所有电机停止
    digitalWrite(PinConfig::L298N_1_IN1, LOW);
    digitalWrite(PinConfig::L298N_1_IN2, LOW);
    digitalWrite(PinConfig::L298N_1_EN, LOW);
    digitalWrite(PinConfig::L298N_2_IN1, LOW);
    digitalWrite(PinConfig::L298N_2_IN2, LOW);
    digitalWrite(PinConfig::L298N_2_EN, LOW);
    
    Serial.println("[电机控制] 引脚初始化完成");
}

/**
 * 纯函数：计算电机方向对应的引脚电平
 * 输入：目标方向
 * 输出：tuple(in1, in2)
 * 无副作用，纯计算
 */
inline auto calculateMotorPins(const MotorDirection dir) -> std::tuple<uint8_t, uint8_t> {
    switch (dir) {
        case MotorDirection::FORWARD:
            return {HIGH, LOW};   // IN1=1, IN2=0 -> 正转
        case MotorDirection::BACKWARD:
            return {LOW, HIGH};   // IN1=0, IN2=1 -> 反转
        case MotorDirection::STOP:
        default:
            return {LOW, LOW};    // IN1=0, IN2=0 -> 停止
    }
}

/**
 * 纯函数：应用单个电机状态到硬件
 * 这是一个"命令"函数，产生副作用（更新硬件）
 * 但逻辑上基于输入状态确定性地执行
 */
inline void applyMotorState(const MotorState& motor) {
    const auto [in1Level, in2Level] = calculateMotorPins(motor.direction);
    
    digitalWrite(motor.pinIn1, in1Level);
    digitalWrite(motor.pinIn2, in2Level);
    
    // 只有在非停止状态下才输出PWM
    if (motor.direction != MotorDirection::STOP) {
        analogWrite(motor.pinEn, motor.speed);
    } else {
        analogWrite(motor.pinEn, 0);
    }
}

/**
 * 纯函数：创建新电机状态（不改变原状态）
 * 输入：原状态，新方向，新速度
 * 输出：新电机状态
 */
inline MotorState createMotorState(
    const uint8_t in1, const uint8_t in2, const uint8_t en,
    const MotorDirection dir, const uint8_t speed
) {
    return MotorState(in1, in2, en, dir, speed);
}

/**
 * 纯函数：创建新电机状态（基于现有引脚配置）
 */
inline MotorState changeMotorState(
    const MotorState& current, const MotorDirection dir, const uint8_t speed
) {
    return MotorState(
        current.pinIn1, current.pinIn2, current.pinEn,
        dir, speed
    );
}

// ============================================
// 高阶函数：运动模式组合
// ============================================

/**
 * 纯函数：创建基础停止状态
 */
inline VehicleMotion createStopState() {
    return VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN, 
                   MotorDirection::STOP, 0),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN, 
                   MotorDirection::STOP, 0),
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN, 
                   MotorDirection::STOP, 0),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN, 
                   MotorDirection::STOP, 0)
    );
}

/**
 * 纯函数：前进状态
 * 输入：速度值
 * 输出：整车运动状态
 */
inline VehicleMotion createForwardState(const uint8_t speed) {
    return VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::FORWARD, speed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::FORWARD, speed),
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::FORWARD, speed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::FORWARD, speed)
    );
}

/**
 * 纯函数：后退状态
 */
inline VehicleMotion createBackwardState(const uint8_t speed) {
    return VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::BACKWARD, speed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::BACKWARD, speed),
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::BACKWARD, speed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::BACKWARD, speed)
    );
}

/**
 * 纯函数：左转状态（差速转弯）
 * 原理：左侧电机后退，右侧电机前进，或左侧速度低于右侧
 */
inline VehicleMotion createLeftTurnState(const uint8_t speed) {
    return VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::BACKWARD, (speed + 1) / 2),  // 左侧慢速后退（奇数保持对称）
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::FORWARD, speed),         // 右侧正常前进
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::BACKWARD, (speed + 1) / 2),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::FORWARD, speed)
    );
}

/**
 * 纯函数：右转状态（差速转弯）
 */
inline VehicleMotion createRightTurnState(const uint8_t speed) {
    return VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::FORWARD, speed),         // 左侧正常前进
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::BACKWARD, (speed + 1) / 2),  // 右侧慢速后退（奇数保持对称）
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::FORWARD, speed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::BACKWARD, (speed + 1) / 2)
    );
}

/**
 * 纯函数：原地左转（左右轮反向）
 */
inline VehicleMotion createInPlaceLeftState(const uint8_t speed) {
    return VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::BACKWARD, speed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::FORWARD, speed),
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::BACKWARD, speed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::FORWARD, speed)
    );
}

/**
 * 纯函数：原地右转（左右轮反向）
 */
inline VehicleMotion createInPlaceRightState(const uint8_t speed) {
    return VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::FORWARD, speed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::BACKWARD, speed),
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   MotorDirection::FORWARD, speed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   MotorDirection::BACKWARD, speed)
    );
}

/**
 * 高阶函数：应用整车运动状态到所有电机
 * 输入：整车运动状态
 * 副作用：更新所有电机硬件
 */
inline void applyVehicleMotion(const VehicleMotion& motion) {
    applyMotorState(motion.frontLeft);
    applyMotorState(motion.frontRight);
    applyMotorState(motion.rearLeft);
    applyMotorState(motion.rearRight);
}

/**
 * 纯函数：创建差速运动状态（左右轮不同速度）
 * 用于PID修正后的精确控制
 * 输入：左电机方向+速度，右电机方向+速度
 * 输出：整车运动状态
 */
inline VehicleMotion createDifferentialState(
    MotorDirection leftDir, uint8_t leftSpeed,
    MotorDirection rightDir, uint8_t rightSpeed
) {
    return VehicleMotion(
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   leftDir, leftSpeed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   rightDir, rightSpeed),
        MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                   leftDir, leftSpeed),
        MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                   rightDir, rightSpeed)
    );
}

// ============================================
// 命令解析函数：从WASD映射到运动状态
// ============================================

/**
 * 纯函数：将WASD命令字符映射到运动状态
 * 输入：命令字符（W/A/S/D/空格）
 * 输入：速度值
 * 输出：对应的整车运动状态
 */
inline VehicleMotion parseWASDCommand(const char cmd, const uint8_t speed) {
    switch (cmd) {
        case 'W': case 'w':
            return createForwardState(speed);
        case 'S': case 's':
            return createBackwardState(speed);
        case 'A': case 'a':
            return createLeftTurnState(speed);
        case 'D': case 'd':
            return createRightTurnState(speed);
        case 'Q': case 'q':
            return createInPlaceLeftState(speed);
        case 'E': case 'e':
            return createInPlaceRightState(speed);
        case ' ':  // 空格键停止
        default:
            return createStopState();
    }
}

#endif // MOTOR_CONTROL_H