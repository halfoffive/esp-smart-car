/**
 * 无线通信系统 - 函数式编程风格
 * 基于 ESP32-C6，使用 ESP-NOW 协议进行低延迟通信
 * 支持命令传输和状态反馈
 * 作者：智能车项目团队
 * 版本：1.2.0
 */

#ifndef WIRELESS_H
#define WIRELESS_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

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
    SERVO = 2,       // 舵机控制
    SPEED = 3,       // 速度设置
    LIGHT = 4,       // 车灯控制
    HORN = 5,        // 喇叭
    STOP = 6,        // 紧急停止
    STATUS = 7,      // 状态查询
    ODOMETRY = 8,    // 测速数据上报
    CALIBRATE = 9,   // 校准命令
    DRIVE_MODE = 10  // 行走模式切换
};

/**
 * 无线数据包结构体
 * 固定大小，确保传输效率
 */
struct __attribute__((packed)) WirelessPacket {
    const uint8_t magic;        // 魔术字（0xA5）用于帧同步
    const uint8_t version;      // 协议版本
    const CommandType type;     // 命令类型
    const uint8_t data;         // 数据字节（如WASD命令或角度）
    const uint8_t speed;        // 速度值
    const uint16_t seq;         // 序列号
    const uint8_t checksum;     // 校验和
    
    // 构造函数
    constexpr WirelessPacket(
        uint8_t m, uint8_t v, CommandType t,
        uint8_t d, uint8_t s, uint16_t sq, uint8_t c
    ) : magic(m), version(v), type(t), data(d), speed(s), seq(sq), checksum(c) {}
};

/**
 * 测速数据上报结构体
 * 用于向PC端发送左右轮速度数据
 * 方向：车载端 -> 接收器 -> PC
 */
struct __attribute__((packed)) OdometryPacket {
    const uint8_t magic;            // 魔术字(0xA5)
    const uint8_t version;          // 协议版本
    const CommandType type;         // ODOMETRY
    const int16_t leftSpeedMmps;    // 左轮速度(mm/s)，有符号
    const int16_t rightSpeedMmps;   // 右轮速度(mm/s)，有符号
    const int16_t headingX100;      // 航向角(弧度*100)，有符号
    const uint16_t totalDistMm;     // 总行走距离(mm)
    const uint8_t checksum;         // 校验和
    
    constexpr OdometryPacket(
        uint8_t m, uint8_t v, CommandType t,
        int16_t ls, int16_t rs, int16_t hd, uint16_t td, uint8_t c
    ) : magic(m), version(v), type(t), leftSpeedMmps(ls),
        rightSpeedMmps(rs), headingX100(hd), totalDistMm(td), checksum(c) {}
};

/**
 * 设备角色枚举
 */
enum class DeviceRole : uint8_t {
    CAR = 0,        // 车载端
    RECEIVER = 1,   // 接收器端
    CAMERA = 2      // 摄像头端
};

/**
 * 通信状态
 */
struct WirelessState {
    const DeviceRole role;           // 本机角色
    const bool isConnected;            // 是否已连接
    const uint8_t peerCount;         // 已配对设备数
    const uint16_t lastSeq;          // 最后接收序列号
    const uint32_t lastRecvTime;     // 最后接收时间
    const uint32_t txCount;          // 发送计数
    const uint32_t rxCount;          // 接收计数
    const uint32_t errCount;         // 错误计数
    
    constexpr WirelessState(
        DeviceRole r, bool c, uint8_t pc,
        uint16_t ls, uint32_t lrt, uint32_t tx, uint32_t rx, uint32_t err
    ) : role(r), isConnected(c), peerCount(pc),
        lastSeq(ls), lastRecvTime(lrt), txCount(tx), rxCount(rx), errCount(err) {}
};

// ============================================
// 常量定义
// ============================================
namespace WirelessConfig {
    constexpr uint8_t MAGIC_BYTE = 0xA5;      // 帧同步魔术字
    constexpr uint8_t PROTOCOL_VERSION = 1;   // 协议版本
    constexpr uint8_t MAX_PEERS = 5;          // 最大配对设备数
    constexpr uint32_t TIMEOUT_MS = 500;        // 超时时间（毫秒）
    constexpr uint8_t CHANNEL = 1;            // 通信信道
    
    // 接收器MAC地址（需与接收器固件一致）
    constexpr uint8_t RECEIVER_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    // 车载端MAC地址
    constexpr uint8_t CAR_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    // 摄像头MAC地址
    constexpr uint8_t CAMERA_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};
}

// ============================================
// 全局状态（可修改，非纯函数）
// ============================================
// 注意：这些用于存储回调中的状态
static WirelessState g_wirelessState(
    DeviceRole::CAR, false, 0, 0, 0, 0, 0, 0
);
static esp_now_peer_info_t g_peerInfo;

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
 * 纯函数：创建命令数据包
 * 输入：命令类型，数据，速度
 * 输出：数据包
 */
inline WirelessPacket createCommandPacket(
    CommandType type, uint8_t data, uint8_t speed
) {
    static uint16_t seqCounter = 0;
    
    WirelessPacket packet(
        WirelessConfig::MAGIC_BYTE,
        WirelessConfig::PROTOCOL_VERSION,
        type,
        data,
        speed,
        ++seqCounter,
        0  // 校验和先设为0
    );
    
    // 计算校验和
    const uint8_t checksum = calculateChecksum(packet);
    
    return WirelessPacket(
        packet.magic, packet.version, packet.type,
        packet.data, packet.speed, packet.seq,
        checksum
    );
}

/**
 * 纯函数：创建运动命令包
 */
inline WirelessPacket createMovePacket(uint8_t wasdCmd, uint8_t speed) {
    return createCommandPacket(CommandType::MOVE, wasdCmd, speed);
}

/**
 * 纯函数：创建舵机命令包
 */
inline WirelessPacket createServoPacket(uint8_t servoCmd, uint8_t speed) {
    return createCommandPacket(CommandType::SERVO, servoCmd, speed);
}

/**
 * 纯函数：创建停止命令包
 */
inline WirelessPacket createStopPacket() {
    return createCommandPacket(CommandType::STOP, 0, 0);
}

/**
 * 纯函数：创建状态查询包
 */
inline WirelessPacket createStatusPacket() {
    return createCommandPacket(CommandType::STATUS, 0, 0);
}

// ============================================
// 回调函数（非纯函数，处理硬件事件）
// ============================================

/**
 * 发送回调函数
 * 当数据发送完成时调用
 */
inline void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        // 发送成功
    } else {
        // 发送失败
        // 注意：此处无法直接修改const字段，实际实现需使用可变状态
    }
}

/**
 * 接收回调函数
 * 当接收到数据时调用
 */
inline void onDataRecv(const uint8_t* mac, const uint8_t* incomingData, int len) {
    if (len != sizeof(WirelessPacket)) {
        return;  // 数据长度不匹配
    }
    
    const WirelessPacket* packet = reinterpret_cast<const WirelessPacket*>(incomingData);
    
    if (!validatePacket(*packet)) {
        return;  // 校验失败
    }
    
    // 处理有效数据包
    // 实际实现：将命令放入队列，由主循环处理
}

// ============================================
// 初始化函数（包含副作用）
// ============================================

/**
 * 初始化ESP-NOW
 * 输入：本机角色
 * 输出：是否成功
 */
inline bool initializeWireless(DeviceRole role) {
    // 初始化WiFi
    WiFi.mode(WIFI_STA);
    
    // 设置本机MAC地址（根据角色）
    switch (role) {
        case DeviceRole::CAR:
            // 设置车载端MAC地址
            break;
        case DeviceRole::RECEIVER:
            // 设置接收器MAC地址
            break;
        case DeviceRole::CAMERA:
            // 设置摄像头MAC地址
            break;
    }
    
    // 初始化ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[无线通信] ESP-NOW 初始化失败");
        return false;
    }
    
    // 注册回调函数
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);
    
    // 添加配对设备
    memset(&g_peerInfo, 0, sizeof(g_peerInfo));
    
    switch (role) {
        case DeviceRole::CAR:
            // 车载端配对接收器
            memcpy(g_peerInfo.peer_addr, WirelessConfig::RECEIVER_MAC, 6);
            break;
        case DeviceRole::RECEIVER:
            // 接收器配对车载端
            memcpy(g_peerInfo.peer_addr, WirelessConfig::CAR_MAC, 6);
            break;
        case DeviceRole::CAMERA:
            // 摄像头配对接收器
            memcpy(g_peerInfo.peer_addr, WirelessConfig::RECEIVER_MAC, 6);
            break;
    }
    
    g_peerInfo.channel = WirelessConfig::CHANNEL;
    g_peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&g_peerInfo) != ESP_OK) {
        Serial.println("[无线通信] 添加配对设备失败");
        return false;
    }
    
    Serial.println("[无线通信] ESP-NOW 初始化成功");
    return true;
}

/**
 * 发送数据包
 * 输入：目标MAC，数据包
 * 输出：是否发送成功
 */
inline bool sendPacket(const uint8_t* peerMac, const WirelessPacket& packet) {
    // 使用局部缓冲区拷贝 MAC，避免 const_cast 修改只读数据
    uint8_t macBuffer[6];
    memcpy(macBuffer, peerMac, 6);
    
    const esp_err_t result = esp_now_send(
        macBuffer,
        reinterpret_cast<const uint8_t*>(&packet),
        sizeof(packet)
    );
    
    return result == ESP_OK;
}

/**
 * 通用原始数据发送函数
 * 用于发送非 WirelessPacket 类型的数据（如 OdometryPacket）
 */
inline bool sendRawPacket(const uint8_t* peerMac, const uint8_t* data, size_t len) {
    uint8_t macBuffer[6];
    memcpy(macBuffer, peerMac, 6);
    
    const esp_err_t result = esp_now_send(macBuffer, data, len);
    return result == ESP_OK;
}

/**
 * 发送命令到接收器
 */
inline bool sendToReceiver(const WirelessPacket& packet) {
    return sendPacket(WirelessConfig::RECEIVER_MAC, packet);
}

/**
 * 发送命令到车载端
 */
inline bool sendToCar(const WirelessPacket& packet) {
    return sendPacket(WirelessConfig::CAR_MAC, packet);
}

/**
 * 发送命令到摄像头
 */
inline bool sendToCamera(const WirelessPacket& packet) {
    return sendPacket(WirelessConfig::CAMERA_MAC, packet);
}

#endif // WIRELESS_H
