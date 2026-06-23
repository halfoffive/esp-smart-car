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
 * 说明：
 * - 接收器以 WiFi AP-only 模式运行，车载端/摄像头作为 STA 接入，
 *   无需接收器连接外部网络。
 *
 * 作者：智能车项目团队
 * 版本：2.2.0（整帧单包协议：简化视频处理，移除分包组装逻辑）
 * 日期：2026-06-20
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
    constexpr uint32_t SERIAL_BAUD = 3000000;  // 串口波特率（USB-CDC 虚拟串口，拉满吞吐）
    constexpr uint32_t BUFFER_SIZE = 32768;    // 缓冲区大小（32KB，匹配后端帧上限）
    constexpr uint32_t LINK_STATUS_INTERVAL = 5000; // 链路状态上报间隔（5秒）
    constexpr uint32_t MAX_SERIAL_WRITE_WAIT_MS = 30; // 串口写出保底超时（毫秒，批量写出路径不触发）
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

/// BLE 扫描是否正在进行（可能在 BLE 回调中修改，使用 volatile）
volatile bool g_bleScanning = false;

/// BLE 扫描是否完成（非阻塞扫描模式下由回调置位）
static volatile bool g_bleScanComplete = false;

/// 上次收到车载 UDP 数据的时间戳（0 表示从未收到）
static uint32_t g_lastCarDataTime = 0;

/// 动态记录的车载端 IP（默认 0.0.0.0，表示未记录，回退到固定 CAR_IP）
static IPAddress g_carIp;

/// 上次发送链路状态的时间戳
static uint32_t g_lastLinkStatus = 0;

/// 视频/串口转发统计计数器
static uint32_t g_videoPacketsReceived = 0;
static uint32_t g_videoFramesForwarded = 0;
static uint32_t g_serialBytesWritten = 0;
static uint32_t g_lastCounterLogTime = 0;

/// UDP 控制端口对象（接收器 -> 车载）
/// 该 socket 仅用于发送控制包，本地端口不影响功能
WiFiUDP g_udpControl;

/// UDP 遥测端口对象（车载 -> 接收器）
WiFiUDP g_udpTelemetry;

/// UDP 视频端口对象（摄像头 -> 接收器）
WiFiUDP g_udpVideo;

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
 *       增加帧同步：维护一个滑动窗口，找到 MAGIC_BYTE 后尝试解析；
 *       若校验失败，仅丢弃当前 MAGIC_BYTE 而非整包，实现单字节重同步。
 */
inline bool readSerialPacket(WirelessPacket& packet) {
    static uint8_t s_buf[sizeof(WirelessPacket)];
    static size_t s_len = 0;

    // 从串口填充缓冲区
    while (s_len < sizeof(WirelessPacket) && Serial.available() > 0) {
        s_buf[s_len++] = static_cast<uint8_t>(Serial.read());
    }

    while (s_len > 0) {
        // 查找魔术字位置
        size_t magicPos = 0;
        while (magicPos < s_len && s_buf[magicPos] != WirelessConfig::MAGIC_BYTE) {
            magicPos++;
        }

        // 丢弃魔术字之前的字节
        if (magicPos > 0) {
            memmove(s_buf, s_buf + magicPos, s_len - magicPos);
            s_len -= magicPos;
            continue;
        }

        // 数据不足，等待下次轮询
        if (s_len < sizeof(WirelessPacket)) {
            return false;
        }

        memcpy(&packet, s_buf, sizeof(WirelessPacket));
        if (validatePacket(packet)) {
            // 消费完整数据包
            memmove(s_buf, s_buf + sizeof(WirelessPacket), s_len - sizeof(WirelessPacket));
            s_len -= sizeof(WirelessPacket);
            return true;
        }

        // 校验失败：只丢弃开头的魔术字，保留剩余字节继续同步
        memmove(s_buf, s_buf + 1, s_len - 1);
        s_len--;
    }

    return false;
}

// ============================================
// 命令转发
// ============================================

/**
 * 转发二进制 WirelessPacket 到车载控制器（通过 UDP 控制端口）
 * 优先使用从 telemetry 动态记录的车载端 IP，未记录时回退到固定 CAR_IP。
 */
inline void forwardToCar(const WirelessPacket& packet) {
    if (WiFi.softAPgetStationNum() == 0) {
        return;  // 无 STA 连接，直接返回
    }

    IPAddress carIp = g_carIp;
    if (carIp == IPAddress(0, 0, 0, 0)) {
        carIp = IPAddress(NetworkConfig::CAR_IP[0], NetworkConfig::CAR_IP[1], NetworkConfig::CAR_IP[2], NetworkConfig::CAR_IP[3]);
    }
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

/// BLE 扫描结果缓冲区（静态生命周期，供回调使用，避免栈对象析构后悬挂指针）
static BleScanResult g_bleScanResult;

/// BLE 扫描完成回调（非阻塞扫描结束后由 NimBLE 调用）
static void onBleScanComplete(BLEScanResults results) {
    g_bleScanComplete = true;
    g_bleScanning = false;
    (void)results;  // 结果已通过 MyBLEScanCallback 收集
}

/// BLE 扫描回调实例（静态生命周期）
static MyBLEScanCallback g_bleScanCallback(g_bleScanResult);

/**
 * 执行 BLE 扫描
 * 现在只扫描普通 BLE 设备，不再用于发现小车
 * （小车使用固定的 AP/STA 链路，无需通过 BLE 发现）
 * 使用非阻塞扫描：start() 立即返回，扫描完成后通过回调输出结果
 */
void performBleScan() {
    if (g_bleScanning) {
        Serial.println("{\"t\":\"ble\",\"error\":\"scan_in_progress\"}");
        return;
    }

    g_bleScanning = true;
    g_bleScanComplete = false;
    g_bleScanResult.count = 0;
    Serial.println("[BLE] 开始扫描...");

    // BLEDevice::init 只需执行一次，在 setup() 中已完成初始化
    // 重复调用可能导致资源泄漏或状态异常
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(&g_bleScanCallback);
    pBLEScan->setActiveScan(true);
    if (!pBLEScan->start(10, onBleScanComplete)) {
        Serial.println("{\"t\":\"ble\",\"error\":\"start_failed\"}");
        g_bleScanning = false;
        g_bleScanComplete = false;
    }
}

/**
 * 输出 JSON 字符串时对特殊字符进行完整转义
 * 处理：" \\ \b \f \n \r \t 及控制字符 \u00xx
 */
inline void printJsonEscaped(const char* str) {
    for (const char* p = str; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
            case '"':  Serial.print("\\\""); break;
            case '\\': Serial.print("\\\\"); break;
            case '\b': Serial.print("\\b");  break;
            case '\f': Serial.print("\\f");  break;
            case '\n': Serial.print("\\n");  break;
            case '\r': Serial.print("\\r");  break;
            case '\t': Serial.print("\\t");  break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    Serial.print(buf);
                } else {
                    Serial.print(*p);
                }
                break;
        }
    }
}

/**
 * 输出 BLE 扫描结果 JSON
 * 非阻塞扫描完成后在 loop() 中调用
 */
inline void sendBleScanResult() {
    if (!g_bleScanComplete) {
        return;
    }
    g_bleScanComplete = false;

    // 输出 JSON 格式结果
    // 格式: {"t":"ble","devices":[{"name":"xxx","mac":"AA:BB:CC:DD:EE:FF","rssi":-42},...]}
    Serial.print("{\"t\":\"ble\",\"devices\":[");
    for (uint8_t i = 0; i < g_bleScanResult.count; i++) {
        if (i > 0) Serial.print(",");
        const BleDeviceInfo& dev = g_bleScanResult.devices[i];
        Serial.print("{\"name\":\"");
        printJsonEscaped(dev.name);
        Serial.printf("\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d}",
                      dev.mac[0], dev.mac[1], dev.mac[2],
                      dev.mac[3], dev.mac[4], dev.mac[5],
                      dev.rssi);
    }
    Serial.println("]}");

    Serial.printf("[BLE] 扫描完成，发现 %d 个设备\n", g_bleScanResult.count);
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
 * 处理整帧视频包（新协议：整帧单包传输）
 * 帧格式：[0xAA 0x55 0xAA 0x55][帧大小(2字节小端)][帧数据]
 * 返回 true 表示成功处理一个完整视频帧。
 */
inline bool handleVideoPacket(const uint8_t* data, int len) {
    // 整帧最小长度：4字节帧头 + 2字节大小 + 1字节数据 = 7字节
    if (len < 7) return false;

    // 校验帧头标记
    if (data[0] != 0xAA || data[1] != 0x55 ||
        data[2] != 0xAA || data[3] != 0x55) {
        return false;  // 帧头不匹配，丢弃
    }

    // 解析帧大小（小端 uint16）
    const uint16_t frameSize = static_cast<uint16_t>(data[4]) |
                               (static_cast<uint16_t>(data[5]) << 8);

    // 校验帧大小边界
    if (frameSize == 0 || frameSize > ReceiverConfig::BUFFER_SIZE) {
        return false;
    }

    // 校验实际接收长度是否匹配（6字节头 + frameSize）
    if (len < static_cast<int>(6 + frameSize)) {
        return false;  // 数据不完整，丢弃
    }

    // 统计接收到的视频帧
    g_videoPacketsReceived++;

    // 通过USB串口发送完整帧
    // 格式: [0xAA][0x55][帧大小(4字节，小端)][帧数据]
    g_videoFramesForwarded++;
    const uint8_t header[] = {0xAA, 0x55};
    Serial.write(header, 2);
    // 帧大小按小端字节序写入（ESP32 为小端架构）
    Serial.write(reinterpret_cast<const uint8_t*>(&frameSize), 4);
    g_serialBytesWritten += 6;

    // 批量写出帧数据（非阻塞：USB-CDC 缓冲足够容纳 1.4KB 帧，无需分块轮询）
    const size_t written = Serial.write(data + 6, frameSize);
    g_serialBytesWritten += written;
    if (written < frameSize) {
        Serial.printf("[视频] 批量写出不完整: %u/%u 字节\n", written, frameSize);
    }

    return true;
}

// ============================================
// UDP 遥测处理
// ============================================

/**
 * 处理来自车载端的 UDP 遥测数据（仅里程计，视频已移至独立 VIDEO_PORT）
 */
void handleTelemetryPacket() {
    int len = g_udpTelemetry.parsePacket();
    if (len <= 0) return;

    uint8_t buf[256];
    // 超大包丢弃而非截断：清空 UDP 当前包并返回
    if (len > static_cast<int>(sizeof(buf))) {
        while (g_udpTelemetry.available() > 0) {
            g_udpTelemetry.read(buf, sizeof(buf));
        }
        return;
    }
    g_udpTelemetry.read(buf, len);

    // 里程计包
    if (len >= static_cast<int>(sizeof(OdometryPacket))) {
        const OdometryPacket* odomPacket = reinterpret_cast<const OdometryPacket*>(buf);
        if (odomPacket->magic == WirelessConfig::MAGIC_BYTE &&
            odomPacket->version == WirelessConfig::PROTOCOL_VERSION &&
            odomPacket->type == CommandType::ODOMETRY) {
            // 计算并验证校验和（除 checksum 字段外的所有字节）
            uint8_t checksum = 0;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(odomPacket);
            for (size_t i = 0; i < sizeof(OdometryPacket) - 1; i++) {
                checksum += p[i];
            }
            if (checksum != odomPacket->checksum) {
                return;  // 校验失败，丢弃遥测包
            }

            // 收到有效车载遥测数据，动态记录车载端 IP
            g_carIp = g_udpTelemetry.remoteIP();
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

/**
 * 处理来自摄像头的 UDP 视频数据（独立 VIDEO_PORT）
 */
void handleVideoUdp() {
    int len = g_udpVideo.parsePacket();
    if (len <= 0) return;

    uint8_t buf[1024];  // 扩容以容纳 512B 数据包（10B头+512B数据+1B校验和=523B）
    // 超大包丢弃而非截断：清空 UDP 当前包并返回
    if (len > static_cast<int>(sizeof(buf))) {
        while (g_udpVideo.available() > 0) {
            g_udpVideo.read(buf, sizeof(buf));
        }
        return;
    }
    g_udpVideo.read(buf, len);

    if (handleVideoPacket(buf, len)) {
        // 有效视频包也来自车载端，动态记录 IP
        g_carIp = g_udpVideo.remoteIP();
        g_lastCarDataTime = millis();
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
    Serial.println("版本: 2.1.0");
    Serial.println("================================\n");
    
    // AP-only 模式：本设备作为 Soft-AP，等待车载端 STA 接入
    WiFi.mode(WIFI_AP);
    IPAddress apIp(NetworkConfig::AP_IP[0], NetworkConfig::AP_IP[1], NetworkConfig::AP_IP[2], NetworkConfig::AP_IP[3]);
    // 网关复用 AP_IP（AP-only 模式下二者相同）
    IPAddress gateway(NetworkConfig::AP_IP[0], NetworkConfig::AP_IP[1], NetworkConfig::AP_IP[2], NetworkConfig::AP_IP[3]);
    IPAddress subnet(NetworkConfig::SUBNET[0], NetworkConfig::SUBNET[1], NetworkConfig::SUBNET[2], NetworkConfig::SUBNET[3]);
    WiFi.softAPConfig(apIp, gateway, subnet);
    if (!WiFi.softAP(NetworkConfig::AP_SSID, NetworkConfig::AP_PASSWORD)) {
        Serial.println("[WiFi_AP] 启动热点失败，重启中...");
        delay(2000);
        ESP.restart();
    }
    WiFi.setTxPower(WIFI_POWER_20dBm);
    Serial.printf("[WiFi_AP] 热点已启动: %s, 密码: %s, IP: %s, 发射功率: 20 dBm\n",
                  NetworkConfig::AP_SSID, NetworkConfig::AP_PASSWORD, WiFi.softAPIP().toString().c_str());
    
    // 启动 UDP 服务器
    g_udpControl.begin(UdpConfig::CONTROL_PORT);
    g_udpTelemetry.begin(UdpConfig::TELEMETRY_PORT);
    g_udpVideo.begin(UdpConfig::VIDEO_PORT);
    Serial.printf("[UDP] 控制端口 %d，遥测端口 %d，视频端口 %d 已启动\n",
                  UdpConfig::CONTROL_PORT, UdpConfig::TELEMETRY_PORT, UdpConfig::VIDEO_PORT);
    
    // 打印 MAC 地址（优先使用 AP MAC）
    String mac = WiFi.softAPmacAddress();
    if (mac == "00:00:00:00:00:00") {
        mac = WiFi.macAddress();
    }
    Serial.print("[初始化] MAC: ");
    Serial.println(mac);
    
    // 初始化 BLE（扫描前只需初始化一次）
    BLEDevice::init("智能车");

    // 视频缓冲区 g_videoBuffer 为全局静态对象，构造函数已完成初始化

    Serial.println("[初始化] 接收器就绪，等待命令...");
    Serial.println("[命令格式] 串口输入已统一为二进制 WirelessPacket");
}

// ============================================
// 主循环
// ============================================

void loop() {
    // 1. 处理串口输入（二进制 WirelessPacket）
    if (Serial.available() >= static_cast<int>(sizeof(WirelessPacket))) {
        WirelessPacket packet{};  // aggregate initialization，删除构造函数后的调用方式
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

    // 2. 处理车载 UDP 遥测数据（里程计）
    handleTelemetryPacket();

    // 2.1 处理车载 UDP 视频数据（独立 VIDEO_PORT）
    handleVideoUdp();

    // 2.5 输出非阻塞 BLE 扫描结果（如有）
    sendBleScanResult();

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

    // 4.5 周期性输出视频转发统计（每 5 秒）
    if (currentTime - g_lastCounterLogTime > ReceiverConfig::LINK_STATUS_INTERVAL) {
        g_lastCounterLogTime = currentTime;
        Serial.printf("[STATS] packets=%u frames=%u bytes=%u\n",
                      g_videoPacketsReceived, g_videoFramesForwarded, g_serialBytesWritten);
        g_videoPacketsReceived = 0;
        g_videoFramesForwarded = 0;
        g_serialBytesWritten = 0;
    }

    // 5. 小延迟
    delay(1);
}
