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
 * 摄像头 -> 接收器: ESP-NOW (视频帧)
 * 接收器 -> 电脑: USB 串口 (视频帧)
 * 
 * 作者：智能车项目团队
 * 版本：1.2.0
 */

#include "../libraries/wireless_protocol/src/wireless.h"
#include <BLEDevice.h>
#include <BLEScan.h>

// ============================================
// 常量定义
// ============================================
namespace ReceiverConfig {
    constexpr uint32_t SERIAL_BAUD = 921600;   // 串口波特率（高速传输）
    constexpr uint32_t BUFFER_SIZE = 32768;    // 缓冲区大小（32KB，匹配后端帧上限）
    constexpr uint32_t HEARTBEAT_INTERVAL = 1000; // 心跳间隔
}

// ============================================
// 数据结构
// ============================================

/**
 * 串口命令结构
 */
struct SerialCommand {
    char cmd;                // 命令字符（非 const：允许赋值操作）
    uint8_t speed;           // 速度值（非 const：允许赋值操作）
    bool isValid;            // 是否有效（非 const：允许赋值操作）
    
    constexpr SerialCommand(char c, uint8_t s, bool v)
        : cmd(c), speed(s), isValid(v) {}
};

/**
 * 视频帧缓冲区（使用静态数组，避免 new/delete 内存泄漏）
 */
struct VideoFrameBuffer {
    uint8_t data[ReceiverConfig::BUFFER_SIZE]; // 静态数组，无需动态分配
    size_t size;             // 当前大小
    size_t capacity;         // 容量
    uint16_t frameId;        // 帧序号
    uint16_t packetsReceived; // 已接收包数
    uint16_t totalPackets;   // 总包数
    bool isComplete;         // 是否完整
    
    VideoFrameBuffer() : size(0), capacity(ReceiverConfig::BUFFER_SIZE),
                         frameId(0), packetsReceived(0), totalPackets(0), isComplete(false) {}
};

/**
 * BLE 设备信息
 */
struct BleDeviceInfo {
    char name[32];          // 设备名称
    uint8_t mac[6];         // MAC 地址
    int8_t rssi;            // 信号强度
    bool isValid;           // 是否有效
};

/**
 * BLE 扫描结果
 */
struct BleScanResult {
    BleDeviceInfo devices[20];  // 最多存储 20 个设备
    uint8_t count;              // 设备数量

    BleScanResult() : count(0) {}
};

// ============================================
// 全局状态
// ============================================
VideoFrameBuffer g_videoBuffer;
uint32_t g_lastHeartbeat = 0;

/// BLE 扫描是否正在进行
bool g_bleScanning = false;

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
 * - Q/E: 原地旋转
 * - 空格: 停止
 * - 1-9: 速度设置
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
            return SerialCommand(input, 0, true);
        case 'M': case 'm':
            return SerialCommand(input, 0, true);
        case 'T': case 't':  // 行走模式切换（专属命令字节，与 MAC_CONFIG 的 'M' 不冲突）
            return SerialCommand(input, 0, true);
        case 'B': case 'b':  // BLE 扫描
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
        case 'D': case 'd':      // 'D' 是右转（MOVE），不是云台
        case 'Q': case 'q':
        case 'E': case 'e':
        case ' ':
            return CommandType::MOVE;
        case '1': case '2': case '3':
        case '4': case '5': case '6':
        case '7': case '8': case '9':
            return CommandType::SPEED;
        case 'M': case 'm':
            return CommandType::MAC_CONFIG;
        case 'T': case 't':  // 行走模式切换（专属命令字节，与 MAC_CONFIG 的 'M' 不冲突）
            return CommandType::DRIVE_MODE;
        default:
            return CommandType::NONE;
    }
}

// ============================================
// 命令转发
// ============================================

/**
 * 从串口读取MAC地址（新帧格式：0xFF帧边界 + 长度字节 + MAC字节）
 * 输入：目标缓冲区（至少6字节）
 * 输出：是否成功在超时内读取完毕
 *
 * 帧格式：'M' + 0xFF + 长度(6) + 6字节MAC
 * 防止 MAC 字节恰好匹配控制字符导致误动作
 */
inline bool readMacBytes(uint8_t* macBuffer) {
    constexpr uint32_t MAC_READ_TIMEOUT_MS = 100;
    const uint32_t startTime = millis();

    // 读取帧边界标识 0xFF
    while (millis() - startTime < MAC_READ_TIMEOUT_MS) {
        if (Serial.available()) {
            int marker = Serial.read();
            if (marker == 0xFF) {
                break;
            }
            // 非 0xFF 字节，忽略（可能是残留数据）
        }
    }
    if (millis() - startTime >= MAC_READ_TIMEOUT_MS) {
        Serial.println("[接收器] MAC帧边界标识超时");
        return false;
    }

    // 读取长度字节
    while (millis() - startTime < MAC_READ_TIMEOUT_MS) {
        if (Serial.available()) {
            int len = Serial.read();
            if (len != 6) {
                Serial.printf("[接收器] MAC长度异常: %d（期望6）\n", len);
                return false;
            }
            break;
        }
    }
    if (millis() - startTime >= MAC_READ_TIMEOUT_MS) {
        Serial.println("[接收器] MAC长度字节超时");
        return false;
    }

    // 读取6字节MAC地址
    for (uint8_t i = 0; i < 6; i++) {
        const uint32_t byteStart = millis();
        while (millis() - byteStart < MAC_READ_TIMEOUT_MS) {
            if (Serial.available()) {
                macBuffer[i] = static_cast<uint8_t>(Serial.read());
                break;
            }
        }
        if (millis() - byteStart >= MAC_READ_TIMEOUT_MS) {
            Serial.printf("[接收器] MAC字节%d读取超时\n", i);
            return false;
        }
    }
    return true;
}

/**
 * 转发命令到车载控制器
 */
inline void forwardToCar(const SerialCommand& cmd) {
    const CommandType type = getCommandType(cmd.cmd);
    if (type == CommandType::NONE) return;

    // MAC地址配置命令：读取后续6字节并更新目标MAC
    if (type == CommandType::MAC_CONFIG) {
        uint8_t mac[6];
        if (readMacBytes(mac)) {
            setTargetCarMac(mac);
            Serial.print("[MAC配置] 车载端MAC已更新: ");
            for (int i = 0; i < 6; i++) {
                if (i > 0) Serial.print(':');
                Serial.printf("%02X", mac[i]);
            }
            Serial.println();
        } else {
            Serial.println("[MAC配置] 读取MAC地址超时");
        }
        return;
    }

    // 行走模式切换命令：读取后续1字节模式值并转发
    if (type == CommandType::DRIVE_MODE) {
        // 读取模式值（1字节：0=普通, 1=直线修正, 2=航向锁定）
        // 使用超时等待，防止模式值字节尚未到达时被静默丢弃
        constexpr uint32_t MODE_READ_TIMEOUT_MS = 50;
        const uint32_t modeStart = millis();
        int modeVal = -1;
        while (millis() - modeStart < MODE_READ_TIMEOUT_MS) {
            if (Serial.available()) {
                modeVal = Serial.read();
                break;
            }
        }
        if (modeVal >= 0) {
            // 使用 createCommandPacket 确保 magic/version/checksum 正确设置
            WirelessPacket pkt = createCommandPacket(CommandType::DRIVE_MODE, static_cast<uint8_t>(modeVal), 0);
            sendPacket(WirelessConfig::CAR_MAC, pkt);
        } else {
            Serial.println("[接收器] DRIVE_MODE 模式值读取超时");
        }
        return;
    }

    WirelessPacket packet;

    if (type == CommandType::MOVE) {
        packet = createMovePacket(cmd.cmd, 0);
    } else if (type == CommandType::SPEED) {
        packet = createCommandPacket(CommandType::SPEED, 0, cmd.speed);
    } else {
        return;
    }

    sendToCar(packet);
}

// ============================================
// BLE 扫描
// ============================================

/**
 * BLE 扫描回调类
 * 收集发现的设备信息，按 MAC 地址去重
 */
class MyBLEScanCallback : public BLEAdvertisedDeviceCallbacks {
private:
    BleScanResult& result_;

public:
    MyBLEScanCallback(BleScanResult& result) : result_(result) {}

    void onResult(BLEAdvertisedDevice advertisedDevice) {
        if (result_.count >= 20) return;  // 缓冲区已满

        // 获取 MAC 地址
        BLEAddress addr = advertisedDevice.getAddress();
        const uint8_t* mac = addr.getNative();

        // 按 MAC 去重
        for (uint8_t i = 0; i < result_.count; i++) {
            if (memcmp(result_.devices[i].mac, mac, 6) == 0) {
                // 更新 RSSI（取更强的信号）
                if (advertisedDevice.getRSSI() > result_.devices[i].rssi) {
                    result_.devices[i].rssi = advertisedDevice.getRSSI();
                }
                return;
            }
        }

        // 新设备
        BleDeviceInfo& dev = result_.devices[result_.count];
        memcpy(dev.mac, mac, 6);
        dev.rssi = advertisedDevice.getRSSI();
        dev.isValid = true;

        // 获取设备名称
        if (advertisedDevice.haveName()) {
            strncpy(dev.name, advertisedDevice.getName().c_str(), sizeof(dev.name) - 1);
            dev.name[sizeof(dev.name) - 1] = '\0';
        } else {
            strncpy(dev.name, "Unknown", sizeof(dev.name) - 1);
        }

        result_.count++;
    }
};

/**
 * 执行 BLE 扫描
 * 扫描 10 秒后输出结果到串口
 */
void performBleScan() {
    if (g_bleScanning) {
        Serial.println("{\"t\":\"ble\",\"error\":\"scan_in_progress\"}");
        return;
    }

    g_bleScanning = true;
    Serial.println("[BLE] 开始扫描...");

    BleScanResult scanResult;

    // BLEDevice::init 只需执行一次，在 setup() 中已完成初始化
    // 重复调用可能导致资源泄漏或状态异常
    BLEScan* pBLEScan = BLEDevice::getScan();
    MyBLEScanCallback callback(scanResult);
    pBLEScan->setAdvertisedDeviceCallbacks(&callback);
    pBLEScan->setActiveScan(true);
    pBLEScan->start(10);  // 扫描 10 秒

    // 输出 JSON 格式结果
    // 格式: {"t":"ble","devices":[{"name":"xxx","mac":"AA:BB:CC:DD:EE:FF","rssi":-42},...]}
    Serial.print("{\"t\":\"ble\",\"devices\":[");
    for (uint8_t i = 0; i < scanResult.count; i++) {
        if (i > 0) Serial.print(",");
        const BleDeviceInfo& dev = scanResult.devices[i];
        Serial.printf("{\"name\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d}",
                      dev.name,
                      dev.mac[0], dev.mac[1], dev.mac[2],
                      dev.mac[3], dev.mac[4], dev.mac[5],
                      dev.rssi);
    }
    Serial.println("]}");

    Serial.printf("[BLE] 扫描完成，发现 %d 个设备\n", scanResult.count);
    g_bleScanning = false;
}

// ============================================
// 视频处理
// ============================================

/**
 * 初始化视频缓冲区（静态数组无需动态分配）
 */
inline void initVideoBuffer() {
    g_videoBuffer.size = 0;
    g_videoBuffer.isComplete = false;
}

/**
 * 处理视频包
 */
inline void handleVideoPacket(const uint8_t* data, int len) {
    // VideoPacket 最小长度：11字节头部 + 1字节数据 + 1字节校验和 = 13字节
    if (len < 13) return;

    const VideoPacket* packet = reinterpret_cast<const VideoPacket*>(data);
    // 严格校验视频包魔术字和版本，防止误判
    if (packet->magic != StreamConfig::VIDEO_MAGIC ||
        packet->version != StreamConfig::PROTOCOL_VERSION) return;
    // 校验 dataLen 边界，防止缓冲区溢出
    if (packet->dataLen > StreamConfig::MAX_PACKET_SIZE) return;
    
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
    
    // 追加数据（帧缓冲区溢出保护）
    // 安全检查：如果当前数据写入会超出缓冲区边界，
    // 丢弃当前帧并重置缓冲区，防止内存越界写入
    if (g_videoBuffer.size + packet->dataLen > g_videoBuffer.capacity) {
        // 缓冲区溢出，丢弃当前帧，重置状态
        g_videoBuffer.size = 0;
        g_videoBuffer.packetsReceived = 0;
        g_videoBuffer.isComplete = false;
        return;
    }
    memcpy(g_videoBuffer.data + g_videoBuffer.size, packet->data, packet->dataLen);
    g_videoBuffer.size += packet->dataLen;
    g_videoBuffer.packetsReceived++;
    
    // 检查帧是否完整
    if (g_videoBuffer.packetsReceived >= g_videoBuffer.totalPackets) {
        g_videoBuffer.isComplete = true;
        
        // 通过USB串口发送完整帧
        // 格式: [0xAA][0x55][帧大小(4字节)][帧数据]
        // Serial缓冲区溢出检查：确保发送空间足够，
        // 否则丢弃当前帧避免阻塞或数据截断
        const size_t totalWriteLen = 2 + 4 + g_videoBuffer.size;  // header + size + data
        if (static_cast<size_t>(Serial.availableForWrite()) >= totalWriteLen) {
            const uint8_t header[] = {0xAA, 0x55};
            Serial.write(header, 2);
            Serial.write(reinterpret_cast<const uint8_t*>(&g_videoBuffer.size), 4);
            Serial.write(g_videoBuffer.data, g_videoBuffer.size);
        }
        // else: Serial缓冲区不足，丢弃当前帧（视频允许丢帧）
        
        g_videoBuffer.isComplete = false;
        g_videoBuffer.size = 0;
    }
}

// ============================================
// ESP-NOW 回调
// ============================================

void onReceiverDataRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
    // 检查是否是视频数据（优先且严格校验）
    // VideoPacket 最小长度：11字节头部 + 1字节数据 + 1字节校验和 = 13字节
    if (len >= 13) {
        const VideoPacket* packet = reinterpret_cast<const VideoPacket*>(data);
        if (packet->magic == StreamConfig::VIDEO_MAGIC &&
            packet->version == StreamConfig::PROTOCOL_VERSION) {
            // 验证校验和（防止传输错误导致花屏）
            // 发送端将校验和置于实际发送数据的最后一字节位置（data[len-1]），
            // 而非 VideoPacket::checksum 字段（该字段在非满载包时不在发送范围内）。
            // 因此必须从 data[len-1] 读取接收到的校验和，而非 packet->checksum。
            uint8_t sum = 0;
            for (int j = 0; j < len - 1; j++) {  // -1 排除校验和字节
                sum += data[j];
            }
            const uint8_t receivedChecksum = data[len - 1];
            if (sum != receivedChecksum) {
                // 校验和不匹配，丢弃损坏的包
                return;
            }
            handleVideoPacket(data, len);
            return;
        }
    }
    
    // 处理测速数据包（OdometryPacket 格式，优先于 WirelessPacket）
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
                         static_cast<unsigned int>(odomPacket->totalDistMm));
            return;
        }
    }
    
    // 处理普通命令（WirelessPacket）
    if (len >= sizeof(WirelessPacket)) {
        const WirelessPacket* packet = reinterpret_cast<const WirelessPacket*>(data);
        if (validatePacket(*packet)) {
            // 处理状态反馈
            if (packet->type == CommandType::STATUS) {
                // 转发状态到电脑
                Serial.write(data, len);
            }
            // 注意：ODOMETRY 类型已由上方 OdometryPacket 分支处理（JSON格式化），
            // 不在此处透传原始二进制数据
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
    Serial.println("版本: 1.2.0");
    Serial.println("================================\n");
    
    // 初始化无线通信
    if (!initializeWireless(DeviceRole::RECEIVER)) {
        Serial.println("[初始化] 无线通信初始化失败，重启中...");
        delay(2000);
        ESP.restart();
    }
    
    // 注册接收回调
    if (esp_now_register_recv_cb(onReceiverDataRecv) != ESP_OK) {
        Serial.println("[无线通信] 注册接收回调失败");
    }
    
    // 初始化 BLE（扫描前只需初始化一次）
    BLEDevice::init("");
    
    // 初始化视频缓冲区
    initVideoBuffer();
    
    Serial.println("[初始化] 接收器就绪，等待命令...");
    Serial.println("[命令格式] WASD:移动, 1-9:速度, 空格:停止, B:蓝牙扫描");
}

// ============================================
// 主循环
// ============================================

void loop() {
    // 1. 处理串口输入
    if (Serial.available()) {
        const int input = Serial.read();
        if (input < 0) return;  // 无数据或读取错误
        const SerialCommand cmd = parseSerialCommand(static_cast<char>(input));
        
        if (cmd.isValid) {
            // BLE 扫描命令：直接执行扫描，不转发到车载端
            if (cmd.cmd == 'B' || cmd.cmd == 'b') {
                performBleScan();
                return;  // 不转发到车载端
            }

            // 转发到车载控制器
            forwardToCar(cmd);
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
