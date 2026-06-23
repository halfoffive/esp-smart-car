/**
 * 无线通信系统 - 应用层数据包格式与 UDP 网络常量
 * 基于 ESP32，使用 WiFi/UDP 进行低延迟通信
 * 支持命令传输、状态反馈和视频流分包
 * 作者：智能车项目团队
 * 版本：2.1.0（修复 P0-01：Wi-Fi 凭据移出本文件至 wifi_credentials.h）
 *
 * 说明：本文件为 Arduino 库，供 car_controller、camera_module、receiver_dongle 共享。
 * 避免复制到各 sketch 目录，减少维护负担。
 */

#ifndef WIRELESS_H
#define WIRELESS_H

#include <Arduino.h>
#include "wifi_credentials.h"

// ============================================
// 纯数据类型定义
// ============================================

/**
 * 命令类型枚举
 * 显式定义所有可传输命令
 */
enum class CommandType : uint8_t {
    NONE = 0,        // 无命令
    MOVE = 1,        // 运动控制
    SPEED = 2,       // 速度设置（speed 字段为 0-255 PWM）
    LIGHT = 3,       // 车灯控制
    HORN = 4,        // 喇叭
    STOP = 5,        // 紧急停止
    STATUS = 6,      // 状态查询
    ODOMETRY = 7,    // 测速数据上报
    CALIBRATE = 8,   // 校准命令
    DRIVE_MODE = 9,  // 行走模式切换
    BLE_SCAN = 10,   // 接收器本地：BLE 扫描（不转发到车载端）
    LINK_STATUS = 11 // 接收器本地：链路状态探测（不转发到车载端）
};

/**
 * 无线数据包结构体
 * 固定大小，确保传输效率
 */
struct __attribute__((packed)) WirelessPacket {
    uint8_t magic;        // 魔术字（0xA5）用于帧同步
    uint8_t version;      // 协议版本
    CommandType type;     // 命令类型
    uint8_t data;         // 数据字节（如WASD命令或角度/模式值）
    uint8_t speed;        // 速度值：直接表示 PWM 占空比，范围 0-255
    // 注意：seq 字段未按 16 位边界对齐；ESP32 可处理，但可移植性降低
    uint16_t seq;         // 序列号
    uint8_t checksum;     // 校验和
};

/**
 * 测速数据上报结构体
 * 用于向PC端发送左右轮速度数据
 * 方向：车载端 -> 接收器 -> PC
 */
struct __attribute__((packed)) OdometryPacket {
    uint8_t magic;            // 魔术字(0xA5)
    uint8_t version;          // 协议版本
    CommandType type;         // ODOMETRY
    // 注意：以下 int16/uint16 字段在 packed 结构体中未按 16 位边界对齐；ESP32 可处理，但可移植性降低
    int16_t leftSpeedMmps;    // 左轮速度(mm/s)，有符号
    int16_t rightSpeedMmps;   // 右轮速度(mm/s)，有符号
    int16_t headingX100;      // 航向角(弧度*100)，有符号
    uint16_t totalDistMm;     // 总行走距离(mm)
    uint8_t checksum;         // 校验和
};

/**
 * 视频数据包
 * 用于分包传输大帧
 */
struct __attribute__((packed)) VideoPacket {
    uint8_t magic;        // 魔术字 0xA6
    uint8_t version;      // 版本
    // 注意：以下 uint16 字段在 packed 结构体中未按 16 位边界对齐；ESP32 可处理，但可移植性降低
    uint16_t frameId;     // 帧序号
    uint16_t packetId;    // 包序号
    uint16_t totalPackets; // 总包数
    uint16_t dataLen;     // 数据长度
    uint8_t data[512];    // 数据（最大512字节，减少分包数降低UDP开销）
    uint8_t checksum;     // 校验和
};

// ============================================
// 常量定义
// ============================================
namespace WirelessConfig {
    constexpr uint8_t MAGIC_BYTE = 0xA5;      // 帧同步魔术字
    constexpr uint8_t PROTOCOL_VERSION = 1;   // 协议版本
}

namespace StreamConfig {
    constexpr uint8_t VIDEO_MAGIC = 0xA6;       // 视频帧魔术字
    constexpr uint8_t PROTOCOL_VERSION = 1;   // 协议版本
    constexpr uint16_t MAX_PACKET_SIZE = 512; // 每包最大数据量（与 VideoPacket.data[512] 对齐）
    constexpr uint16_t TARGET_FPS = 10;       // 目标帧率（10 FPS，QVGA 320x240 下充裕发送窗口）
    constexpr uint32_t FRAME_INTERVAL = 1000 / TARGET_FPS; // 帧间隔
    constexpr uint8_t JPEG_QUALITY_MIN = 12;  // 最小压缩值=最高质量
    constexpr uint8_t JPEG_QUALITY_MAX = 63;  // 最大压缩值=最低质量（ESP32 驱动上限，应对复杂场景）
}

namespace UdpConfig {
    constexpr uint16_t CONTROL_PORT = 9000;   // 控制命令 UDP 端口
    constexpr uint16_t TELEMETRY_PORT = 9001; // 遥测数据 UDP 端口
    constexpr uint16_t VIDEO_PORT = 9002;     // 视频流 UDP 端口（与遥测分离）
}

namespace NetworkConfig {
    constexpr const char* AP_SSID = WIFI_AP_SSID;        // 软接入点 SSID（来自 wifi_credentials.h）
    constexpr const char* AP_PASSWORD = WIFI_AP_PASSWORD; // 软接入点密码（来自 wifi_credentials.h）
    constexpr uint8_t AP_IP[4] = {192, 168, 4, 1};       // 接入点 IP
    constexpr uint8_t CAR_IP[4] = {192, 168, 4, 2};      // 车载端固定 IP
    // 注意：GATEWAY 复用 AP_IP，接入点模式下网关与 AP_IP 相同
    constexpr const uint8_t (&GATEWAY)[4] = AP_IP;       // 默认网关
    constexpr uint8_t SUBNET[4] = {255, 255, 255, 0};    // 子网掩码
}

// ============================================
// 纯函数：数据包操作
// ============================================

/**
 * 纯函数：计算校验和
 * 输入：数据包
 * 输出：校验和
 */
inline uint8_t calculateChecksum(const WirelessPacket& packet) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&packet);
    uint8_t sum = 0;
    // 计算除校验和字段外的所有字节
    for (size_t i = 0; i < sizeof(packet) - 1; i++) {
        sum += data[i];
    }
    return sum;
}

/**
 * 纯函数：验证数据包
 * 输入：数据包
 * 输出：是否有效
 */
inline bool validatePacket(const WirelessPacket& packet) {
    return packet.magic == WirelessConfig::MAGIC_BYTE &&
           packet.version == WirelessConfig::PROTOCOL_VERSION &&
           packet.checksum == calculateChecksum(packet);
}

/**
 * 创建命令数据包（纯函数：序列号由调用方传入）
 * 输入：命令类型，数据，速度，序列号
 * 输出：数据包
 */
inline WirelessPacket createCommandPacket(
    CommandType type, uint8_t data, uint8_t speed, uint16_t seq
) {
    WirelessPacket packet{};
    packet.magic = WirelessConfig::MAGIC_BYTE;
    packet.version = WirelessConfig::PROTOCOL_VERSION;
    packet.type = type;
    packet.data = data;
    packet.speed = speed;
    packet.seq = seq;
    packet.checksum = calculateChecksum(packet);
    return packet;
}

/**
 * 纯函数：创建运动命令包
 */
inline WirelessPacket createMovePacket(uint8_t wasdCmd, uint8_t speed, uint16_t seq) {
    return createCommandPacket(CommandType::MOVE, wasdCmd, speed, seq);
}

/**
 * 纯函数：创建停止命令包
 */
inline WirelessPacket createStopPacket(uint16_t seq) {
    return createCommandPacket(CommandType::STOP, 0, 0, seq);
}

/**
 * 纯函数：创建状态查询包
 */
inline WirelessPacket createStatusPacket(uint16_t seq) {
    return createCommandPacket(CommandType::STATUS, 0, 0, seq);
}

#endif // WIRELESS_H
