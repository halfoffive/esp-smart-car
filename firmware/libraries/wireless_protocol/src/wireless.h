/**
 * 无线通信系统 - 函数式编程风格
 * 基于 ESP32，使用 ESP-NOW 协议进行低延迟通信
 * 支持命令传输、状态反馈和视频流分包
 * 作者：智能车项目团队
 * 版本：1.2.0
 * 
 * 说明：本文件为 Arduino 库，供 car_controller、camera_module、receiver_dongle 共享。
 * 避免复制到各 sketch 目录，减少维护负担。
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
    DRIVE_MODE = 10, // 行走模式切换
    MAC_CONFIG = 11  // MAC地址配置
};

/**
 * 无线数据包结构体
 * 固定大小，确保传输效率
 */
struct __attribute__((packed)) WirelessPacket {
    uint8_t magic;        // 魔术字（0xA5）用于帧同步
    uint8_t version;      // 协议版本
    CommandType type;     // 命令类型
    uint8_t data;         // 数据字节（如WASD命令或角度）
    uint8_t speed;        // 速度值
    uint16_t seq;         // 序列号
    uint8_t checksum;     // 校验和
    
    // 默认构造函数（用于声明未初始化的局部变量，后续赋值）
    WirelessPacket() : magic(0), version(0), type(CommandType::NONE),
                       data(0), speed(0), seq(0), checksum(0) {}
    
    // 带参构造函数
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
    uint8_t magic;            // 魔术字(0xA5)
    uint8_t version;          // 协议版本
    CommandType type;         // ODOMETRY
    int16_t leftSpeedMmps;    // 左轮速度(mm/s)，有符号
    int16_t rightSpeedMmps;   // 右轮速度(mm/s)，有符号
    int16_t headingX100;      // 航向角(弧度*100)，有符号
    uint16_t totalDistMm;     // 总行走距离(mm)
    uint8_t checksum;         // 校验和
    
    constexpr OdometryPacket(
        uint8_t m, uint8_t v, CommandType t,
        int16_t ls, int16_t rs, int16_t hd, uint16_t td, uint8_t c
    ) : magic(m), version(v), type(t), leftSpeedMmps(ls),
        rightSpeedMmps(rs), headingX100(hd), totalDistMm(td), checksum(c) {}
};

/**
 * 视频数据包
 * 用于分包传输大帧
 */
struct __attribute__((packed)) VideoPacket {
    uint8_t magic;        // 魔术字 0xA6
    uint8_t version;      // 版本
    uint16_t frameId;     // 帧序号
    uint16_t packetId;    // 包序号
    uint16_t totalPackets; // 总包数
    uint16_t dataLen;     // 数据长度
    uint8_t data[128];    // 数据（最大128字节）
    uint8_t checksum;     // 校验和
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
    DeviceRole role;           // 本机角色
    uint8_t peerCount;         // 已配对设备数
    
    constexpr WirelessState(
        DeviceRole r, uint8_t pc
    ) : role(r), peerCount(pc) {}
};

// ============================================
// 常量定义
// ============================================
namespace WirelessConfig {
    constexpr uint8_t MAGIC_BYTE = 0xA5;      // 帧同步魔术字
    constexpr uint8_t PROTOCOL_VERSION = 1;   // 协议版本
    constexpr uint8_t MAX_PEERS = 5;          // 最大配对设备数
    constexpr uint8_t CHANNEL = 1;            // 通信信道
    
    // 接收器MAC地址（需与接收器固件一致）
    inline uint8_t RECEIVER_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    // 车载端MAC地址（非 const，支持运行时修改）
    inline uint8_t CAR_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    // 摄像头MAC地址（非 const，支持运行时修改）
    inline uint8_t CAMERA_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};
}

namespace StreamConfig {
    constexpr uint8_t VIDEO_MAGIC = 0xA6;       // 视频帧魔术字
    constexpr uint8_t PROTOCOL_VERSION = 1;   // 协议版本
    constexpr uint8_t MAX_PACKET_SIZE = 128;   // 每包最大数据量
    constexpr uint16_t TARGET_FPS = 30;       // 目标帧率
    constexpr uint32_t FRAME_INTERVAL = 1000 / TARGET_FPS; // 帧间隔
    constexpr uint8_t JPEG_QUALITY_MIN = 5;   // 最小JPEG质量
    constexpr uint8_t JPEG_QUALITY_MAX = 50;  // 最大JPEG质量
}

// ============================================
// 全局状态（可修改，非纯函数）
// ============================================
// 注意：使用 inline 确保头文件被多个翻译单元包含时只有一个定义
inline WirelessState g_wirelessState(
    DeviceRole::CAR, 0
);
inline esp_now_peer_info_t g_peerInfo{};

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
 * 创建命令数据包（非纯函数，内部维护序列号计数器）
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
// 回调函数声明（由各 sketch 自行实现并注册）
// ============================================

/**
 * 发送回调函数声明
 * 各 sketch 可定义自己的实现
 */
extern void onDataSent(const wifi_tx_info_t* info, esp_now_send_status_t status);

/**
 * 接收回调函数声明
 * 各 sketch 必须定义自己的实现
 */
extern void onDataRecv(const esp_now_recv_info* info, const uint8_t* incomingData, int len);

// ============================================
// 初始化函数（包含副作用）
// ============================================

/**
 * 初始化ESP-NOW
 * 输入：本机角色
 * 输出：是否成功
 * 
 * 注意：本函数不注册回调。各 sketch 应在 setup() 中自行调用
 * esp_now_register_send_cb() 和 esp_now_register_recv_cb() 注册自己的回调。
 */
inline bool initializeWireless(DeviceRole role) {
    // 初始化WiFi
    WiFi.mode(WIFI_STA);
    
    // 初始化ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[无线通信] ESP-NOW 初始化失败");
        return false;
    }
    
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
    
    // 接收器需要额外配对摄像头，用于转发云台控制等命令
    if (role == DeviceRole::RECEIVER) {
        memset(&g_peerInfo, 0, sizeof(g_peerInfo));
        memcpy(g_peerInfo.peer_addr, WirelessConfig::CAMERA_MAC, 6);
        g_peerInfo.channel = WirelessConfig::CHANNEL;
        g_peerInfo.encrypt = false;
        
        if (esp_now_add_peer(&g_peerInfo) != ESP_OK) {
            Serial.println("[无线通信] 添加摄像头配对设备失败");
            return false;
        }
        Serial.println("[无线通信] 摄像头配对设备添加成功");
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
 * 用于发送非 WirelessPacket 类型的数据（如 OdometryPacket、VideoPacket）
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

/**
 * 设置目标车载端MAC地址
 * 输入：6字节MAC地址指针
 */
inline void setTargetCarMac(const uint8_t* newMac) {
    // 保存旧 MAC，删除旧 peer，再添加新 peer
    // esp_now_mod_peer 按 peer_addr 查找，用新 MAC 查不到旧 peer，需先删后加
    uint8_t oldMac[6];
    memcpy(oldMac, WirelessConfig::CAR_MAC, 6);
    
    if (esp_now_del_peer(oldMac) != ESP_OK) {
        Serial.println("[无线通信] 删除旧车载端配对信息失败");
    }
    
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, newMac, 6);
    peerInfo.channel = WirelessConfig::CHANNEL;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("[无线通信] 添加新车载端配对信息失败");
    }
    
    memcpy(WirelessConfig::CAR_MAC, newMac, 6);
}

/**
 * 设置目标摄像头MAC地址
 * 输入：6字节MAC地址指针
 */
inline void setTargetCameraMac(const uint8_t* newMac) {
    // 保存旧 MAC，删除旧 peer，再添加新 peer
    // esp_now_mod_peer 按 peer_addr 查找，用新 MAC 查不到旧 peer，需先删后加
    uint8_t oldMac[6];
    memcpy(oldMac, WirelessConfig::CAMERA_MAC, 6);
    
    if (esp_now_del_peer(oldMac) != ESP_OK) {
        Serial.println("[无线通信] 删除旧摄像头配对信息失败");
    }
    
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, newMac, 6);
    peerInfo.channel = WirelessConfig::CHANNEL;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("[无线通信] 添加新摄像头配对信息失败");
    }
    
    memcpy(WirelessConfig::CAMERA_MAC, newMac, 6);
}

#endif // WIRELESS_H
