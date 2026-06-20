/**
 * 电脑端接收器主程序 - ESP32-C6
 * 基于函数式编程思想
 * 
 * 功能：
 * 1. 通过 WiFi AP + UDP 接收摄像头视频帧
 * 2. 通过 USB 串口传输视频到电脑
 * 3. 接收电脑控制命令并通过 UDP 转发到车载控制器
 * 4. 支持命令路由和状态反馈
 * 
 * 硬件接线：ESP32-C6 官方开发版
 * - USB 连接到电脑
 * - 无线通信（WiFi AP + UDP）
 * 
 * 通信协议：
 * 电脑 -> 接收器: 二进制 WirelessPacket（串口，sizeof(WirelessPacket) 字节）
 * 接收器 -> 车载: WiFi UDP
 * 摄像头 -> 接收器: WiFi UDP (视频帧)
 * 接收器 -> 电脑: USB 串口 (视频帧)
 * 
 * 作者：智能车项目团队
 * 版本：2.0.0
 */

#include "../libraries/wireless_protocol/src/wireless.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <BLEDevice.h>
#include <BLEScan.h>

// ============================================
// 常量定义
// ============================================
namespace ReceiverConfig {
    constexpr uint32_t SERIAL_BAUD = 921600;   // 串口波特率（高速传输）
    constexpr uint32_t BUFFER_SIZE = 32768;    // 缓冲区大小（32KB，匹配后端帧上限）
    constexpr uint32_t LINK_STATUS_INTERVAL = 5000; // 链路状态上报间隔（5秒）
}

// ============================================
// 数据结构
// ============================================

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
    uint8_t mac[6];         // BLE MAC 地址（扫描到的广播地址）
    uint8_t wifiMac[6];     // WiFi MAC 地址（从 Manufacturer Data 提取，保留兼容）
    bool hasWifiMac;        // 是否包含 WiFi MAC
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

/// BLE 扫描是否正在进行
bool g_bleScanning = false;

/// 上次收到车载 UDP 数据的时间戳（0 表示从未收到）
static uint32_t g_lastCarDataTime = 0;

/// 上次发送链路状态的时间戳
static uint32_t g_lastLinkStatus = 0;

/// UDP 控制端口对象（接收器 -> 车载）
WiFiUDP g_udpControl;

/// UDP 遥测端口对象（车载 -> 接收器）
WiFiUDP g_udpTelemetry;

/// 当前已连接 STA 数量（用于输出连接/断开日志）
static uint8_t g_lastStationCount = 0;

// ============================================
// 二进制数据包读取
// ============================================

/**
 * 从串口读取一个 WirelessPacket
 * 输入：packet 引用
 * 输出：true 表示成功读取并校验通过（magic/version/checksum）
 * 
 * 说明：串口协议已统一为二进制 WirelessPacket，与 UDP 控制载荷格式一致。
 *       读取字节数为 sizeof(WirelessPacket)（当前为 8 字节），由 packed 结构体决定。
 */
inline bool readSerialPacket(WirelessPacket& packet) {
    if (Serial.available() < static_cast<int>(sizeof(WirelessPacket))) {
        return false;
    }

    uint8_t buffer[sizeof(WirelessPacket)];
    Serial.readBytes(buffer, sizeof(WirelessPacket));
    memcpy(&packet, buffer, sizeof(WirelessPacket));

    return validatePacket(packet);
}

// ============================================
// 命令转发
// ============================================

/**
 * 转发二进制 WirelessPacket 到车载控制器（通过 UDP 控制端口）
 */
inline void forwardToCar(const WirelessPacket& packet) {
    IPAddress carIp(NetworkConfig::CAR_IP[0], NetworkConfig::CAR_IP[1], NetworkConfig::CAR_IP[2], NetworkConfig::CAR_IP[3]);
    g_udpControl.beginPacket(carIp, UdpConfig::CONTROL_PORT);
    g_udpControl.write(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    if (!g_udpControl.endPacket()) {
        Serial.println("[UDP] 控制包发送失败");
    }
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
        dev.hasWifiMac = false;  // 默认无 WiFi MAC

        // 尝试从 Manufacturer Data 提取 WiFi MAC（保留兼容）
        // 车载 C6 广播格式: [Company ID 2字节=0xFFFF] + [WiFi MAC 6字节]
        // NimBLE 的 getManufacturerData() 返回 Arduino String，非 std::string
        if (advertisedDevice.haveManufacturerData()) {
            String mfgData = advertisedDevice.getManufacturerData();
            if (mfgData.length() >= 8) {
                // 前 2 字节为 Company ID（应为 0xFF 0xFF），跳过
                // 后 6 字节为 WiFi MAC
                memcpy(dev.wifiMac, mfgData.c_str() + 2, 6);
                dev.hasWifiMac = true;
            }
        }

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
 * 现在只扫描普通 BLE 设备，不再用于发现小车
 * （小车使用固定的 AP/STA 链路，无需通过 BLE 发现）
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
    // 格式: {"t":"ble","devices":[{"name":"xxx","mac":"AA:BB:CC:DD:EE:FF","rssi":-42,"wifi_mac":"AA:BB:CC:DD:EE:FF"},...]}
    // wifi_mac 仅当设备广播了 Manufacturer Data 且包含 WiFi MAC 时才会出现
    Serial.print("{\"t\":\"ble\",\"devices\":[");
    for (uint8_t i = 0; i < scanResult.count; i++) {
        if (i > 0) Serial.print(",");
        const BleDeviceInfo& dev = scanResult.devices[i];
        Serial.printf("{\"name\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d",
                      dev.name,
                      dev.mac[0], dev.mac[1], dev.mac[2],
                      dev.mac[3], dev.mac[4], dev.mac[5],
                      dev.rssi);
        // 如果有 WiFi MAC，追加到 JSON 中
        if (dev.hasWifiMac) {
            Serial.printf(",\"wifi_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"",
                          dev.wifiMac[0], dev.wifiMac[1], dev.wifiMac[2],
                          dev.wifiMac[3], dev.wifiMac[4], dev.wifiMac[5]);
        }
        Serial.print("}");
    }
    Serial.println("]}");

    Serial.printf("[BLE] 扫描完成，发现 %d 个设备\n", scanResult.count);
    g_bleScanning = false;
}

// ============================================
// 链路状态上报
// ============================================

/**
 * 输出链路状态 JSON
 * 格式: {"t":"link","dongle":"ok","car_paired":true/false,"last_odom_ms":...}
 *
 * 字段说明：
 * - t: 固定 "link"
 * - dongle: 固定 "ok"（dongle 自身总是 ok，否则无法响应）
 * - car_paired: 基于最近 2 秒内是否收到过车载 UDP 数据
 * - last_odom_ms: 距离上次收到车载数据的毫秒数，从未收到则为 -1
 *
 * 触发时机：
 * 1. 收到 'P' 探测命令时立即调用
 * 2. loop() 中每 5 秒周期性调用
 */
inline void sendLinkStatus() {
    // 检查最近 2 秒内是否收到过车载数据
    const bool carPaired = (g_lastCarDataTime != 0) &&
                           (millis() - g_lastCarDataTime < 2000);

    // 计算距离上次收到车载数据的毫秒数
    // g_lastCarDataTime == 0 表示从未收到车载数据，输出 -1
    int32_t lastOdomMs;
    if (g_lastCarDataTime == 0) {
        lastOdomMs = -1;  // 从未收到车载数据
    } else {
        lastOdomMs = static_cast<int32_t>(millis() - g_lastCarDataTime);
    }

    // 输出 JSON 行（带换行符，便于后端按行解析）
    Serial.printf("{\"t\":\"link\",\"dongle\":\"ok\",\"car_paired\":%s,\"last_odom_ms\":%d}\n",
                  carPaired ? "true" : "false",
                  lastOdomMs);
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
    // 校验 dataLen 与实际接收长度 len 的一致性
    // 发送大小 = 10 (header) + dataLen + 1 (checksum) = 11 + dataLen
    // 若 dataLen + 11 > len，说明 dataLen 超过实际数据长度（损坏/篡改包），
    // 后续 memcpy 会读取越界，必须提前拒绝
    if (static_cast<int>(packet->dataLen) + 11 > len) return;
    
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
// UDP 遥测处理
// ============================================

/**
 * 处理来自车载端的 UDP 遥测数据
 * 包括视频分包和里程计数据
 */
void handleTelemetryPacket() {
    int len = g_udpTelemetry.parsePacket();
    if (len <= 0) return;

    uint8_t buf[256];
    if (len > static_cast<int>(sizeof(buf))) {
        len = sizeof(buf);
    }
    g_udpTelemetry.read(buf, len);

    // 视频包：最小 13 字节，且头部匹配视频魔术字与版本
    if (len >= 13 && buf[0] == StreamConfig::VIDEO_MAGIC &&
        buf[1] == StreamConfig::PROTOCOL_VERSION) {
        g_lastCarDataTime = millis();
        handleVideoPacket(buf, len);
        return;
    }

    // 里程计包
    if (len >= static_cast<int>(sizeof(OdometryPacket))) {
        const OdometryPacket* odomPacket = reinterpret_cast<const OdometryPacket*>(buf);
        if (odomPacket->magic == WirelessConfig::MAGIC_BYTE &&
            odomPacket->version == WirelessConfig::PROTOCOL_VERSION &&
            odomPacket->type == CommandType::ODOMETRY) {
            g_lastCarDataTime = millis();
            Serial.printf("{\"t\":\"odom\",\"ls\":%d,\"rs\":%d,\"hd\":%d,\"dist\":%u}\n",
                         odomPacket->leftSpeedMmps,
                         odomPacket->rightSpeedMmps,
                         odomPacket->headingX100,
                         static_cast<unsigned int>(odomPacket->totalDistMm));
            return;
        }
    }

    Serial.printf("[UDP] 收到未知遥测包，长度: %d\n", len);
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
    Serial.println("版本: 2.0.0");
    Serial.println("================================\n");
    
    // 配置 WiFi AP 模式
    WiFi.mode(WIFI_AP);
    IPAddress apIp(NetworkConfig::AP_IP[0], NetworkConfig::AP_IP[1], NetworkConfig::AP_IP[2], NetworkConfig::AP_IP[3]);
    IPAddress gateway(NetworkConfig::GATEWAY[0], NetworkConfig::GATEWAY[1], NetworkConfig::GATEWAY[2], NetworkConfig::GATEWAY[3]);
    IPAddress subnet(NetworkConfig::SUBNET[0], NetworkConfig::SUBNET[1], NetworkConfig::SUBNET[2], NetworkConfig::SUBNET[3]);
    WiFi.softAPConfig(apIp, gateway, subnet);
    if (!WiFi.softAP(NetworkConfig::AP_SSID, NetworkConfig::AP_PASSWORD)) {
        Serial.println("[WiFi_AP] 启动热点失败，重启中...");
        delay(2000);
        ESP.restart();
    }
    WiFi.setTxPower(WIFI_POWER_20dBm);
    Serial.printf("[WiFi_AP] 热点已启动: %s, IP: %s, 发射功率: 20 dBm\n", NetworkConfig::AP_SSID, WiFi.softAPIP().toString().c_str());
    
    // 启动 UDP 服务器
    g_udpControl.begin(UdpConfig::CONTROL_PORT);
    g_udpTelemetry.begin(UdpConfig::TELEMETRY_PORT);
    Serial.printf("[UDP] 控制端口 %d，遥测端口 %d 已启动\n",
                  UdpConfig::CONTROL_PORT, UdpConfig::TELEMETRY_PORT);
    
    // 打印 MAC 地址（优先使用 AP MAC）
    String mac = WiFi.softAPmacAddress();
    if (mac == "00:00:00:00:00:00") {
        mac = WiFi.macAddress();
    }
    Serial.print("[初始化] MAC: ");
    Serial.println(mac);
    
    // 初始化 BLE（扫描前只需初始化一次）
    BLEDevice::init("智能车");
    
    // 初始化视频缓冲区
    initVideoBuffer();
    
    Serial.println("[初始化] 接收器就绪，等待命令...");
    Serial.println("[命令格式] 串口输入已统一为二进制 WirelessPacket");
}

// ============================================
// 主循环
// ============================================

void loop() {
    // 1. 处理串口输入（二进制 WirelessPacket）
    if (Serial.available() >= static_cast<int>(sizeof(WirelessPacket))) {
        WirelessPacket packet;
        if (readSerialPacket(packet)) {
            // 本地命令：不转发到车载端
            if (packet.type == CommandType::BLE_SCAN) {
                performBleScan();
            } else if (packet.type == CommandType::LINK_STATUS) {
                sendLinkStatus();
            } else {
                // 运动/速度/行走模式/停止/状态/校准等命令：转发到车载端
                forwardToCar(packet);
            }
        }
    }
    
    // 2. 处理车载 UDP 遥测数据
    handleTelemetryPacket();
    
    // 3. 检测 STA 连接/断开变化并输出日志
    const uint8_t stationCount = WiFi.softAPgetStationNum();
    if (stationCount != g_lastStationCount) {
        if (stationCount > g_lastStationCount) {
            Serial.printf("[WiFi_AP] 客户端已连接，当前数量: %d\n", stationCount);
        } else {
            Serial.printf("[WiFi_AP] 客户端已断开，当前数量: %d\n", stationCount);
        }
        g_lastStationCount = stationCount;
    }
    
    // 4. 周期性上报链路状态（每 5 秒）
    const uint32_t currentTime = millis();
    if (currentTime - g_lastLinkStatus > ReceiverConfig::LINK_STATUS_INTERVAL) {
        g_lastLinkStatus = currentTime;
        sendLinkStatus();
    }
    
    // 5. 小延迟
    delay(1);
}
