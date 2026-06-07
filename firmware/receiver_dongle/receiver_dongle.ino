/**
 * 电脑端接收器主程序 - ESP32-C6
 * 基于函数式编程思想
 * 
 * 功能：
 * 1. 通过 ESP-NOW 接收摄像头视频帧
 * 2. 通过 USB 串口传输视频到电脑
 * 3. 接收电脑控制命令并转发到车载控制器
 * 4. 支持命令路由和状态反馈
 * 
 * 硬件接线：ESP32-C6 官方开发版
 * - USB 连接到电脑
 * - 无线通信（ESP-NOW）
 * 
 * 通信协议：
 * 电脑 -> 接收器: 串口命令 (WASD等)
 * 接收器 -> 车载: ESP-NOW
 * 接收器 -> 摄像头: ESP-NOW
 * 摄像头 -> 接收器: ESP-NOW (视频帧)
 * 接收器 -> 电脑: USB 串口 (视频帧)
 * 
 * 作者：智能车项目团队
 * 版本：1.0.0
 */

#include "wireless.h"

// ============================================
// 常量定义
// ============================================
namespace ReceiverConfig {
    constexpr uint32_t SERIAL_BAUD = 921600;   // 串口波特率（高速传输）
    constexpr uint32_t BUFFER_SIZE = 4096;     // 缓冲区大小
    constexpr uint32_t HEARTBEAT_INTERVAL = 1000; // 心跳间隔
}

// ============================================
// 数据结构
// ============================================

/**
 * 串口命令结构
 */
struct SerialCommand {
    const char cmd;          // 命令字符
    const uint8_t speed;     // 速度值
    const bool isValid;      // 是否有效
    
    constexpr SerialCommand(char c, uint8_t s, bool v)
        : cmd(c), speed(s), isValid(v) {}
};

/**
 * 视频帧缓冲区
 */
struct VideoFrameBuffer {
    uint8_t* data;           // 数据指针
    size_t size;             // 当前大小
    size_t capacity;         // 容量
    uint16_t frameId;        // 帧序号
    uint16_t packetsReceived; // 已接收包数
    uint16_t totalPackets;   // 总包数
    bool isComplete;         // 是否完整
    
    VideoFrameBuffer() : data(nullptr), size(0), capacity(0),
                         frameId(0), packetsReceived(0), totalPackets(0), isComplete(false) {}
};

// ============================================
// 全局状态
// ============================================
VideoFrameBuffer g_videoBuffer;
bool g_isStreaming = false;
uint32_t g_lastHeartbeat = 0;

// ============================================
// 纯函数：串口命令解析
// ============================================

/**
 * 纯函数：解析串口输入
 * 输入：字节
 * 输出：解析后的命令
 * 
 * 命令格式：
 * - W/A/S/D: 运动控制
 * - U/D/L/R: 云台控制
 * - Q/E: 原地旋转
 * - 空格: 停止
 * - 1-9: 速度设置
 * - C: 云台居中
 */
inline SerialCommand parseSerialCommand(const char input) {
    switch (input) {
        case 'W': case 'w':
        case 'A': case 'a':
        case 'S': case 's':
        case 'D': case 'd':
        case 'Q': case 'q':
        case 'E': case 'e':
        case ' ':  // 停止
        case 'U': case 'u':
        case 'L': case 'l':
        case 'R': case 'r':
        case 'C': case 'c':
            return SerialCommand(input, 0, true);
        case '1': case '2': case '3':
        case '4': case '5': case '6':
        case '7': case '8': case '9':
            return SerialCommand(input, map(input - '0', 1, 9, 28, 255), true);
        default:
            return SerialCommand(0, 0, false);
    }
}

/**
 * 纯函数：确定命令类型
 */
inline CommandType getCommandType(const char cmd) {
    switch (cmd) {
        case 'W': case 'w':
        case 'A': case 'a':
        case 'S': case 's':
        case 'D': case 'd':
        case 'Q': case 'q':
        case 'E': case 'e':
        case ' ':
            return CommandType::MOVE;
        case 'U': case 'u':
        case 'D': case 'd':
        case 'L': case 'l':
        case 'R': case 'r':
        case 'C': case 'c':
            return CommandType::SERVO;
        case '1': case '2': case '3':
        case '4': case '5': case '6':
        case '7': case '8': case '9':
            return CommandType::SPEED;
        default:
            return CommandType::NONE;
    }
}

// ============================================
// 命令转发
// ============================================

/**
 * 转发命令到车载控制器
 */
inline void forwardToCar(const SerialCommand& cmd) {
    const CommandType type = getCommandType(cmd.cmd);
    if (type == CommandType::NONE) return;
    
    WirelessPacket packet;
    
    if (type == CommandType::MOVE) {
        packet = createMovePacket(cmd.cmd, 0);
    } else if (type == CommandType::SERVO) {
        packet = createServoPacket(cmd.cmd, 0);
    } else if (type == CommandType::SPEED) {
        packet = createCommandPacket(CommandType::SPEED, 0, cmd.speed);
    } else {
        return;
    }
    
    sendToCar(packet);
}

/**
 * 转发命令到摄像头
 */
inline void forwardToCamera(const SerialCommand& cmd) {
    const CommandType type = getCommandType(cmd.cmd);
    if (type == CommandType::SERVO) {
        const WirelessPacket packet = createServoPacket(cmd.cmd, 0);
        sendToCamera(packet);
    }
}

// ============================================
// 视频处理
// ============================================

/**
 * 初始化视频缓冲区
 */
inline void initVideoBuffer() {
    g_videoBuffer.capacity = ReceiverConfig::BUFFER_SIZE;
    g_videoBuffer.data = new uint8_t[g_videoBuffer.capacity];
    g_videoBuffer.size = 0;
    g_videoBuffer.isComplete = false;
}

/**
 * 处理视频包
 */
inline void handleVideoPacket(const uint8_t* data, int len) {
    if (len < sizeof(VideoPacket)) return;
    
    const VideoPacket* packet = reinterpret_cast<const VideoPacket*>(data);
    if (packet->magic != StreamConfig::VIDEO_MAGIC) return;
    
    // 新帧开始
    if (packet->packetId == 0) {
        g_videoBuffer.size = 0;
        g_videoBuffer.frameId = packet->frameId;
        g_videoBuffer.totalPackets = packet->totalPackets;
        g_videoBuffer.packetsReceived = 0;
        g_videoBuffer.isComplete = false;
    }
    
    // 检查帧序号
    if (packet->frameId != g_videoBuffer.frameId) return;
    
    // 追加数据
    if (g_videoBuffer.size + packet->dataLen <= g_videoBuffer.capacity) {
        memcpy(g_videoBuffer.data + g_videoBuffer.size, packet->data, packet->dataLen);
        g_videoBuffer.size += packet->dataLen;
        g_videoBuffer.packetsReceived++;
    }
    
    // 检查帧是否完整
    if (g_videoBuffer.packetsReceived >= g_videoBuffer.totalPackets) {
        g_videoBuffer.isComplete = true;
        
        // 通过USB串口发送完整帧
        // 格式: [0xAA][0x55][帧大小(4字节)][帧数据]
        const uint8_t header[] = {0xAA, 0x55};
        Serial.write(header, 2);
        Serial.write(reinterpret_cast<const uint8_t*>(&g_videoBuffer.size), 4);
        Serial.write(g_videoBuffer.data, g_videoBuffer.size);
        
        g_videoBuffer.isComplete = false;
        g_videoBuffer.size = 0;
    }
}

// ============================================
// ESP-NOW 回调
// ============================================

void onReceiverDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    // 检查是否是视频数据
    if (len >= sizeof(VideoPacket)) {
        const VideoPacket* packet = reinterpret_cast<const VideoPacket*>(data);
        if (packet->magic == StreamConfig::VIDEO_MAGIC) {
            handleVideoPacket(data, len);
            return;
        }
    }
    
    // 处理普通命令
    if (len >= sizeof(WirelessPacket)) {
        const WirelessPacket* packet = reinterpret_cast<const WirelessPacket*>(data);
        if (validatePacket(*packet)) {
            // 处理状态反馈和测速数据
            if (packet->type == CommandType::STATUS) {
                // 转发状态到电脑
                Serial.write(data, len);
            }
            // 测速数据：转发到PC端（JSON格式便于后端解析）
            else if (packet->type == CommandType::ODOMETRY) {
                // 从 WirelessPacket 解析测速数据
                // 测速数据存放在 data 字段的扩展中
                // 直接透传原始数据
                Serial.write(data, len);
            }
        }
    }
    
    // 处理测速数据包（OdometryPacket 格式）
    // OdometryPacket 包含左右轮速度、航向角等信息
    if (len >= sizeof(OdometryPacket)) {
        const OdometryPacket* odomPacket = reinterpret_cast<const OdometryPacket*>(data);
        if (odomPacket->magic == WirelessConfig::MAGIC_BYTE && 
            odomPacket->version == WirelessConfig::PROTOCOL_VERSION &&
            odomPacket->type == CommandType::ODOMETRY) {
            // 转发测速数据到PC端，使用JSON格式便于后端解析
            // 格式: {"t":"odom","ls":左速度,"rs":右速度,"hd":航向,"dist":距离}\n
            Serial.printf("{\"t\":\"odom\",\"ls\":%d,\"rs\":%d,\"hd\":%d,\"dist\":%u}\n",
                         odomPacket->leftSpeedMmps,
                         odomPacket->rightSpeedMmps,
                         odomPacket->headingX100,
                         odomPacket->totalDistMm);
        }
    }
}

// ============================================
// 初始化
// ============================================

void setup() {
    // 初始化高速串口
    Serial.begin(ReceiverConfig::SERIAL_BAUD);
    delay(1000);
    
    Serial.println("\n================================");
    Serial.println("智能车接收器 - ESP32-C6");
    Serial.println("版本: 1.0.0");
    Serial.println("================================\n");
    
    // 初始化无线通信
    if (!initializeWireless(DeviceRole::RECEIVER)) {
        Serial.println("[初始化] 无线通信初始化失败，重启中...");
        delay(2000);
        ESP.restart();
    }
    
    // 注册接收回调
    esp_now_register_recv_cb(onReceiverDataRecv);
    
    // 初始化视频缓冲区
    initVideoBuffer();
    
    Serial.println("[初始化] 接收器就绪，等待命令...");
    Serial.println("[命令格式] WASD:移动, U/D/L/R:云台, 1-9:速度, 空格:停止");
}

// ============================================
// 主循环
// ============================================

void loop() {
    // 1. 处理串口输入
    if (Serial.available()) {
        const char input = Serial.read();
        const SerialCommand cmd = parseSerialCommand(input);
        
        if (cmd.isValid) {
            // 转发到车载控制器
            forwardToCar(cmd);
            
            // 如果是云台命令，同时转发到摄像头
            if (getCommandType(cmd.cmd) == CommandType::SERVO) {
                forwardToCamera(cmd);
            }
        }
    }
    
    // 2. 发送心跳包
    const uint32_t currentTime = millis();
    if (currentTime - g_lastHeartbeat > ReceiverConfig::HEARTBEAT_INTERVAL) {
        g_lastHeartbeat = currentTime;
        
        // 发送心跳到车载控制器
        const WirelessPacket heartbeat = createCommandPacket(CommandType::STATUS, 0, 0);
        sendToCar(heartbeat);
    }
    
    // 3. 小延迟
    delay(1);
}
