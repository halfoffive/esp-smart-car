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
 * 版本：3.0.0（分包传输协议：接收 S3 视频 chunk→转发到 Serial→后端重组）
 * 日期：2026-06-26
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
static uint32_t g_videoChunksReceived = 0;
static uint32_t g_videoPacketsReceived = 0;
static uint32_t g_videoFramesForwarded = 0;
static uint32_t g_videoChunksDropped = 0;  // 串口写出超时/失败丢弃的分片数
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
// 视频分包处理
// ============================================

/// 分包协议常量（匹配 S3 端 ChunkProtocol）
namespace VideoChunkConfig {
    constexpr uint8_t MAGIC = 0xCC;        // 分片 Magic 字节
    constexpr size_t HEADER_SIZE = 7;      // magic(1)+frameId(2)+chunkIdx(1)+totalChunks(1)+dataSize(2)
}

/**
 * 处理分包视频 chunk
 * chunk 格式：[0xCC][frameId(2B LE)][chunkIdx(1B)][totalChunks(1B)][dataSize(2B LE)][JPEG分片数据]
 * 串口转发格式：[0xCC][totalBytes(4B LE)][frameId(2B)][chunkIdx(1B)][totalChunks(1B)][dataSize(2B)][data]
 *              其中 totalBytes = 2+1+1+2+dataSize（frameId之后所有字段）
 * 返回 true 表示成功处理
 */
inline bool handleVideoChunk(const uint8_t* data, int len) {
    if (len < static_cast<int>(VideoChunkConfig::HEADER_SIZE)) return false;
    if (data[0] != VideoChunkConfig::MAGIC) return false;

    const uint16_t frameId = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);
    const uint8_t chunkIdx = data[3];
    const uint8_t totalChunks = data[4];
    const uint16_t dataSize = static_cast<uint16_t>(data[5]) | (static_cast<uint16_t>(data[6]) << 8);

    if (dataSize == 0 || static_cast<int>(VideoChunkConfig::HEADER_SIZE + dataSize) > len) {
        return false;
    }
    if (totalChunks == 0 || chunkIdx >= totalChunks) {
        return false;
    }

    g_videoChunksReceived++;

    // 串口转发：[0xCC][totalBytes(4B LE)][frameId(2B)][chunkIdx(1B)][totalChunks(1B)][dataSize(2B)][data]
    // 合并为单 buffer 单次 write，减少系统调用次数；USB-CDC 阻塞超时由 setup() 中 setTxTimeoutMs 控制，
    // 超时返回的字节数小于预期时计为丢片，避免 loop() 长时间阻塞导致 UDP socket 缓冲堆积丢包。
    const uint32_t totalBytes = 2 + 1 + 1 + 2 + dataSize;

    // 单 chunk 数据上限：UDP MTU 1500B - IP/UDP 头 28B ≈ 1472B，预留余量取 1500B
    static uint8_t outBuf[1 + 4 + 2 + 1 + 1 + 2 + 1500];
    size_t pos = 0;
    outBuf[pos++] = VideoChunkConfig::MAGIC;
    memcpy(outBuf + pos, &totalBytes, 4); pos += 4;
    memcpy(outBuf + pos, data + 1, 2); pos += 2;   // frameId
    outBuf[pos++] = data[3];                        // chunkIdx
    outBuf[pos++] = data[4];                        // totalChunks
    memcpy(outBuf + pos, data + 5, 2); pos += 2;    // dataSize
    memcpy(outBuf + pos, data + VideoChunkConfig::HEADER_SIZE, dataSize); pos += dataSize;

    // 单次 write；USB-CDC 主机端不取走数据触发超时时返回值小于 pos，计数并继续，不阻塞 loop()
    const size_t written = Serial.write(outBuf, pos);
    g_serialBytesWritten += written;
    if (written < pos) {
        g_videoChunksDropped++;
    }

    // 每帧最后一个 chunk 到达时计为一次帧转发
    if (chunkIdx == totalChunks - 1) {
        g_videoFramesForwarded++;
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
 * 处理来自摄像头的 UDP 视频数据（独立 VIDEO_PORT，分包传输）
 */
void handleVideoUdp() {
    int len = g_udpVideo.parsePacket();
    if (len <= 0) return;

    g_videoPacketsReceived++;  // 真实统计收到的视频 UDP 分片数（含后续校验失败的）

    static uint8_t buf[1400];  // 单 chunk 最大 ~1400B，无需 32KB
    if (len > static_cast<int>(sizeof(buf))) {
        while (g_udpVideo.available() > 0) {
            g_udpVideo.read(buf, sizeof(buf));
        }
        return;
    }
    g_udpVideo.read(buf, len);

    // 仅接受分包 chunk（0xCC），忽略旧协议整帧（0xAA）
    if (handleVideoChunk(buf, len)) {
        g_carIp = g_udpVideo.remoteIP();
        g_lastCarDataTime = millis();
    } else if (buf[0] == VideoChunkConfig::MAGIC) {
        // magic 匹配但校验失败：累计 5 秒输出诊断
        static uint32_t s_lastRejectLog = 0;
        static uint32_t s_rejectCount = 0;
        s_rejectCount++;
        if (millis() - s_lastRejectLog > 5000) {
            Serial.printf("[视频] 过去 5 秒内丢弃 %u 个无效 chunk（长度=%d，magic=0x%02X）\n",
                         s_rejectCount, len, buf[0]);
            s_rejectCount = 0;
            s_lastRejectLog = millis();
        }
    }
    // 非 0xCC 包静默丢弃（旧协议残留或噪声）
}

// ============================================
// 初始化
// ============================================

void setup() {
    // 初始化高速串口
    Serial.begin(ReceiverConfig::SERIAL_BAUD);
    // USB-CDC 写超时：主机端不主动取数据时避免 write 无限阻塞 loop()，
    // 触发丢片计数而非 UDP socket 缓冲堆积→丢包→帧残缺→延时尖峰
    Serial.setTxTimeoutMs(ReceiverConfig::MAX_SERIAL_WRITE_WAIT_MS);
    delay(1000);
    
    Serial.println("\n================================");
    Serial.println("智能车接收器 - ESP32-C6");
    Serial.println("版本: 3.0.0");
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
    if (!g_udpVideo.begin(UdpConfig::VIDEO_PORT)) {
      Serial.printf("[UDP] 视频端口 %d 绑定失败！\n", UdpConfig::VIDEO_PORT);
    }
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
        Serial.printf("[STATS] chunks=%u packets=%u frames=%u dropped=%u bytes=%u\n",
                      g_videoChunksReceived, g_videoPacketsReceived, g_videoFramesForwarded,
                      g_videoChunksDropped, g_serialBytesWritten);
        g_videoChunksReceived = 0;
        g_videoPacketsReceived = 0;
        g_videoFramesForwarded = 0;
        g_videoChunksDropped = 0;
        g_serialBytesWritten = 0;
    }

    // 5. 小延迟
    delay(1);
}
