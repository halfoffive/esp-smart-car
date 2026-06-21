/**
 * 智能车控制系统 - 函数式编程风格
 * 基于 ESP32-S3（Freenove FNK0085），使用 L298N 驱动左右两侧电机（每侧并联 2 个电机）
 * 作者：智能车项目团队
 * 版本：1.4.0（修复 P3-03 电机死区补偿）
 * 日期：2026-06-20
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
 * 包含2个电机的独立状态（左/右，每路 L298N 通道并联驱动 2 个电机）
 */
struct VehicleMotion {
    MotorState left;    // 左侧电机
    MotorState right;   // 右侧电机

    constexpr VehicleMotion(
        MotorState l, MotorState r
    ) : left(l), right(r) {}
};

// ============================================
// 引脚配置常量（ESP32-S3 引脚分配，避开摄像头 GPIO 4-18 与 SPI Flash GPIO 26-37）
// ============================================
namespace PinConfig {
    // L298N 模块1（控制左侧两个电机）
    constexpr uint8_t L298N_1_IN1 = 38;  // 左侧输入1（S3 GPIO 38）
    constexpr uint8_t L298N_1_IN2 = 39;  // 左侧输入2（S3 GPIO 39）
    constexpr uint8_t L298N_1_EN = 40;   // 左侧使能(PWM)（S3 GPIO 40）

    // L298N 模块2（控制右侧两个电机）
    constexpr uint8_t L298N_2_IN1 = 41;  // 右侧输入1（S3 GPIO 41）
    constexpr uint8_t L298N_2_IN2 = 42;  // 右侧输入2（S3 GPIO 42）
    constexpr uint8_t L298N_2_EN = 21;   // 右侧使能(PWM)（S3 GPIO 21）

    // 别名：左侧电机使用 L298N 模块1
    constexpr uint8_t MOTOR_LEFT_IN1 = L298N_1_IN1;   // 左侧输入1 (GPIO 38)
    constexpr uint8_t MOTOR_LEFT_IN2 = L298N_1_IN2;   // 左侧输入2 (GPIO 39)
    // 别名：右侧电机使用 L298N 模块2
    constexpr uint8_t MOTOR_RIGHT_IN1 = L298N_2_IN1;  // 右侧输入1 (GPIO 41)
    constexpr uint8_t MOTOR_RIGHT_IN2 = L298N_2_IN2;  // 右侧输入2 (GPIO 42)
}

// ============================================
// 电机控制常量
// ============================================
namespace MotorConfig {
    constexpr uint8_t DEADBAND_PWM = 15;  // 电机死区阈值（低于此值输出 0，避免机械不响应）
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
 * 应用单个电机状态到硬件
 * 这是一个"命令"函数，产生副作用（更新硬件）
 * 但逻辑上基于输入状态确定性地执行
 */
inline void applyMotorState(const MotorState& motor) {
    // 根据方向直接设置 IN1/IN2 电平
    switch (motor.direction) {
        case MotorDirection::FORWARD:
            digitalWrite(motor.pinIn1, HIGH);
            digitalWrite(motor.pinIn2, LOW);
            break;
        case MotorDirection::BACKWARD:
            digitalWrite(motor.pinIn1, LOW);
            digitalWrite(motor.pinIn2, HIGH);
            break;
        case MotorDirection::STOP:
        default:
            digitalWrite(motor.pinIn1, LOW);
            digitalWrite(motor.pinIn2, LOW);
            break;
    }

    // 只有在非停止状态下才输出PWM
    if (motor.direction != MotorDirection::STOP) {
        // 死区平滑过渡：将 [DEADBAND_PWM, 255] 线性映射到 [0, 255]，
        // 避免命令速度在死区阈值附近出现 15->16 的突变启动。
        uint8_t effectiveSpeed = 0;
        if (motor.speed > MotorConfig::DEADBAND_PWM) {
            const uint16_t mapped = static_cast<uint16_t>(motor.speed - MotorConfig::DEADBAND_PWM) * 255U
                                    / (255U - MotorConfig::DEADBAND_PWM);
            effectiveSpeed = static_cast<uint8_t>((mapped > 255U) ? 255U : mapped);
        }
        analogWrite(motor.pinEn, effectiveSpeed);
    } else {
        analogWrite(motor.pinEn, 0);
    }
}

// ============================================
// 应用整车运动状态
// ============================================

/**
 * 应用整车运动状态到所有电机
 * 输入：整车运动状态
 * 副作用：更新所有电机硬件
 */
inline void applyVehicleMotion(const VehicleMotion& motion) {
    applyMotorState(motion.left);
    applyMotorState(motion.right);
}

// ============================================
// 命令解析函数：从WASD映射到运动状态
// ============================================

/**
 * 将WASD命令字符映射到运动状态
 * 输入：命令字符（W/A/S/D/Q/E/空格）
 * 输入：速度值
 * 输出：通过 out 返回对应的整车运动状态
 * 返回值：true 表示命令有效，false 表示未知命令（out 保持不变）
 */
inline bool commandToVehicleMotion(const char cmd, const uint8_t speed, VehicleMotion& out) {
    switch (cmd) {
        case 'W': case 'w':
            out = VehicleMotion(
                MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                           MotorDirection::FORWARD, speed),
                MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                           MotorDirection::FORWARD, speed)
            );
            return true;
        case 'S': case 's':
            out = VehicleMotion(
                MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                           MotorDirection::BACKWARD, speed),
                MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                           MotorDirection::BACKWARD, speed)
            );
            return true;
        case 'A': case 'a':
            out = VehicleMotion(
                MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                           MotorDirection::BACKWARD, (speed + 1) / 2),  // 左侧慢速后退（奇数保持对称）
                MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                           MotorDirection::FORWARD, speed)              // 右侧正常前进
            );
            return true;
        case 'D': case 'd':
            out = VehicleMotion(
                MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                           MotorDirection::FORWARD, speed),             // 左侧正常前进
                MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                           MotorDirection::BACKWARD, (speed + 1) / 2)   // 右侧慢速后退（奇数保持对称）
            );
            return true;
        case 'Q': case 'q':
            out = VehicleMotion(
                MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                           MotorDirection::BACKWARD, speed),
                MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                           MotorDirection::FORWARD, speed)
            );
            return true;
        case 'E': case 'e':
            out = VehicleMotion(
                MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                           MotorDirection::FORWARD, speed),
                MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                           MotorDirection::BACKWARD, speed)
            );
            return true;
        case ' ':  // 空格键停止
            out = VehicleMotion(
                MotorState(PinConfig::MOTOR_LEFT_IN1, PinConfig::MOTOR_LEFT_IN2, PinConfig::L298N_1_EN,
                           MotorDirection::STOP, 0),
                MotorState(PinConfig::MOTOR_RIGHT_IN1, PinConfig::MOTOR_RIGHT_IN2, PinConfig::L298N_2_EN,
                           MotorDirection::STOP, 0)
            );
            return true;
        default:
            return false;
    }
}

#endif // MOTOR_CONTROL_H