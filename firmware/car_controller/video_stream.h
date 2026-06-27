/**
 * 视频流传输系统 - 函数式编程风格
 * 基于 ESP32-S3 CAM（Freenove FNK0085），视频帧通过 WiFi UDP 分包直发到接收器
 * 支持动态质量调整、帧率控制和分包传输
 * 
 * 作者：智能车项目团队
 * 版本：3.0.0（分包传输协议：S3 分包→C6 转发→后端重组，QVGA 320×240 @ 10fps）
 * 日期：2026-06-26
 */

#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H

#include <Arduino.h>
#include <WiFiUdp.h>
#include "esp_camera.h"
#include "../libraries/wireless_protocol/src/wireless.h"  // 复用无线通信协议（Arduino 库）

// FW-L2: 视频流调试日志开关，生产环境设为0
#ifndef DEBUG_VIDEO
#define DEBUG_VIDEO 0
#endif

// ============================================
// 纯数据类型定义
// ============================================

/**
 * 视频帧状态
 */
struct FrameState {
    camera_fb_t* frameBuffer;        // 帧缓冲指针（非 const：需要传递给 esp_camera_fb_return 释放）
    size_t frameSize;          // 帧大小
    uint32_t timestamp;        // 时间戳
    uint16_t frameId;          // 帧序号
    bool isValid;              // 是否有效
    
    constexpr FrameState(
        camera_fb_t* fb, size_t sz, uint32_t ts, uint16_t id, bool valid
    ) : frameBuffer(fb), frameSize(sz), timestamp(ts), frameId(id), isValid(valid) {}
};

/**
 * 传输状态（可变结构体，直接修改字段）
 */
struct StreamState {
    bool isStreaming;         // 是否正在流传输
    uint32_t lastFrameTime;   // 最后帧时间
    uint16_t fps;             // 实际帧率
    uint32_t totalFrames;     // 总帧数
    uint32_t droppedFrames;   // 丢弃帧数
    uint32_t bytesSent;       // 发送字节数
};

// ============================================
// 全局状态（可变）
// ============================================
inline StreamState g_streamState{};
inline uint16_t g_frameId = 0;
/// 连续帧捕获失败计数（用于错误恢复）
inline uint8_t g_consecutiveFailures = 0;
/// 视频专用 UDP 对象，由 car_controller.ino 定义（独立于 g_udpTelemetry，避免 Core0/Core1 并发竞态）
extern WiFiUDP g_udpVideo;
/// 连续失败超过此阈值时重启摄像头
constexpr uint8_t CAMERA_RESTART_THRESHOLD = 10;
/// 当前 JPEG 压缩值（供 adjustQuality 渐进调整，初始 40 适配 MTU 1400）
inline uint8_t g_currentQuality = 40;

// ============================================
// 分包传输协议常量
// ============================================
namespace ChunkProtocol {
    /// 分包 Magic 字节（0xCC = "Chunk" 的 C）
    constexpr uint8_t MAGIC = 0xCC;
    /// 每分片包头大小：magic(1) + frameId(2) + chunkIdx(1) + totalChunks(1) + dataSize(2)
    constexpr size_t HEADER_SIZE = 7;
    /// 每分片最大 JPEG 数据量（总包 ≤ 1400B，MTU 安全）
    constexpr size_t MAX_DATA_PER_CHUNK = 1393;
    /// 每 UDP 包最大总大小（≤ WiFi MTU 1460B UDP 载荷）
    constexpr size_t MTU_SAFE_PACKET = 1400;
}

// ============================================
// 纯函数：帧处理
// ============================================

/**
 * 捕获帧（有副作用：硬件采集 + 递增全局帧序号）
 * 输出：帧状态
 */
inline FrameState captureFrame() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == NULL) {
        return FrameState(nullptr, 0, millis(), 0, false);
    }
    
    return FrameState(
        fb, fb->len, millis(), ++g_frameId, true
    );
}

/**
 * 纯函数：释放帧
 */
inline void releaseFrame(const FrameState& frame) {
    if (frame.frameBuffer != nullptr) {
        esp_camera_fb_return(frame.frameBuffer);
    }
}

/**
 * 纯函数：计算帧率（滑动窗口，最近 10 帧平均）
 */
inline uint16_t calculateFPS(const uint32_t lastFrameTime, const uint32_t currentTime) {
    static uint32_t intervals[10];
    static uint8_t idx = 0;
    static uint8_t count = 0;
    static uint32_t sum = 0;

    if (lastFrameTime == 0) {
        return 0;
    }
    const uint32_t diff = currentTime - lastFrameTime;
    if (diff == 0) {
        return 0;
    }

    if (count == 10) {
        sum -= intervals[idx];
    } else {
        count++;
    }
    intervals[idx] = diff;
    sum += diff;
    idx = (idx + 1) % 10;

    if (sum == 0) {
        return 0;
    }
    return static_cast<uint16_t>((1000UL * count) / sum);
}

/**
 * 纯函数：渐进阻尼质量调整
 * 根据帧大小缓慢调整 JPEG 压缩值，避免质量二值振荡 → FB-OVF / 像素块
 * 目标：QVGA 320x240 下每帧控制在 2500-10000 字节（分包后每 chunk ≤1393B，2-8 个 chunk/帧）
 * 
 * 注意：ESP32 摄像头驱动中压缩值越小 = 质量越高 = 帧越大
 */
inline uint8_t adjustQuality(const uint32_t frameSize, const uint8_t currentQuality) {
    constexpr uint32_t TARGET_MAX = 10000;   // 帧上限（QVGA 复杂场景约 10KB，8 chunks，2Mbps WiFi 下发送 ~40ms 仍满足 100ms 端到端）
    constexpr uint32_t TARGET_MIN = 2500;    // 帧下限（QVGA 简单场景约 2.5KB，2 chunks，保证基本画质）
    constexpr uint8_t STEP = 10;             // 每步调整量（快速收敛：40→63 需 3 步）

    if (frameSize > TARGET_MAX) {
        // 帧过大：提高压缩值（向 QUALITY_MAX 方向），每步 +STEP
        return (currentQuality < StreamConfig::JPEG_QUALITY_MAX - STEP)
                   ? currentQuality + STEP
                   : StreamConfig::JPEG_QUALITY_MAX;
    }
    if (frameSize < TARGET_MIN) {
        // 帧过小：降低压缩值（向 QUALITY_MIN 方向），每步 -STEP
        return (currentQuality > StreamConfig::JPEG_QUALITY_MIN + STEP)
                   ? currentQuality - STEP
                   : StreamConfig::JPEG_QUALITY_MIN;
    }
    // 帧大小在目标区间内，保持当前质量
    return currentQuality;
}

// ============================================
// 传输函数（分包发送）
// ============================================

/**
 * 发送完整视频帧（分包传输，由后端重组）
 * 将 JPEG 帧切分为多个 chunk，逐个 UDP 发送到接收器
 * 每 chunk 格式：[0xCC][frameId(2B LE)][chunkIdx(1B)][totalChunks(1B)][dataSize(2B LE)][JPEG分片]
 * 总包大小 ≤ 1400B，MTU 安全
 * S3 单芯片架构下由独立 FreeRTOS 任务调用
 * 协议约束：发送方不保证原子性——任一 chunk 的 beginPacket/endPacket 失败即 return false 并停止发送剩余 chunk，
 *           接收端必须按 frameId 严格重组，frameId 跳变时丢弃旧帧不完整分片。
 * 返回：true 发送成功，false 发送失败
 *
 * FW-M9: 视频分包添加CRC校验涉及协议变更（需要C6接收器、后端重组代码同步修改），
 *        改动复杂且当前UDP校验和已提供一定完整性保护，暂不实现。
 */
inline bool sendVideoFrame(const FrameState& frame) {
    if (!frame.isValid) return false;

    const uint8_t* data = frame.frameBuffer->buf;
    const size_t totalLen = frame.frameSize;

    // 计算分片数
    const uint8_t totalChunks = static_cast<uint8_t>(
        (totalLen + ChunkProtocol::MAX_DATA_PER_CHUNK - 1) / ChunkProtocol::MAX_DATA_PER_CHUNK
    );
    if (totalChunks == 0) return false;

    // 目标地址
    const IPAddress apIp(NetworkConfig::AP_IP[0], NetworkConfig::AP_IP[1],
                         NetworkConfig::AP_IP[2], NetworkConfig::AP_IP[3]);

    // 逐分片发送
    for (uint8_t chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
        const size_t offset = static_cast<size_t>(chunkIdx) * ChunkProtocol::MAX_DATA_PER_CHUNK;
        const uint16_t dataSize = (chunkIdx == totalChunks - 1)
            ? static_cast<uint16_t>(totalLen - offset)
            : static_cast<uint16_t>(ChunkProtocol::MAX_DATA_PER_CHUNK);

        // 构建 UDP 包（BSS 段静态数组，不走 FreeRTOS 任务栈）
        static uint8_t packet[ChunkProtocol::MTU_SAFE_PACKET];
        packet[0] = ChunkProtocol::MAGIC;
        packet[1] = static_cast<uint8_t>(g_frameId & 0xFF);
        packet[2] = static_cast<uint8_t>((g_frameId >> 8) & 0xFF);
        packet[3] = chunkIdx;
        packet[4] = totalChunks;
        packet[5] = static_cast<uint8_t>(dataSize & 0xFF);
        packet[6] = static_cast<uint8_t>((dataSize >> 8) & 0xFF);
        memcpy(packet + ChunkProtocol::HEADER_SIZE, data + offset, dataSize);

        const size_t packetSize = ChunkProtocol::HEADER_SIZE + dataSize;

        if (!g_udpVideo.beginPacket(apIp, UdpConfig::VIDEO_PORT)) {
            static uint32_t s_lastBeginFailLog = 0;
            if (millis() - s_lastBeginFailLog > 3000) {
                Serial.printf("[UDP] 视频chunk beginPacket失败（frameId=%u chunk=%u/%u）\n",
                              g_frameId, chunkIdx + 1, totalChunks);
                s_lastBeginFailLog = millis();
            }
            return false;
        }
        g_udpVideo.write(packet, packetSize);
        if (!g_udpVideo.endPacket()) {
            static uint32_t s_lastEndFailLog = 0;
            if (millis() - s_lastEndFailLog > 3000) {
                Serial.printf("[UDP] 视频chunk endPacket失败（frameId=%u chunk=%u/%u）\n",
                              g_frameId, chunkIdx + 1, totalChunks);
                s_lastEndFailLog = millis();
            }
            return false;
        }
    }

    // FW-L2: 每100帧统计日志仅在DEBUG_VIDEO开启时输出
#if DEBUG_VIDEO
    // 每100帧输出一次统计（避免刷屏）
    static uint32_t s_lastSentLog = 0;
    if (g_frameId % 100 == 0 && g_frameId != s_lastSentLog) {
        s_lastSentLog = g_frameId;
        Serial.printf("[视频流] 帧%u 发送成功 (%u chunks, %uB)\n",
                      g_frameId, totalChunks, static_cast<unsigned int>(totalLen));
    }
#endif

    return true;
}

/**
 * 启动流传输
 */
inline void startStreaming() {
    g_streamState = {};
    g_streamState.isStreaming = true;
    Serial.println("[视频流] 开始传输（分包模式：S3分包→C6转发→后端重组）");
}

/**
 * 停止流传输
 */
inline void stopStreaming() {
    g_streamState.isStreaming = false;
    Serial.println("[视频流] 停止传输");
}

/**
 * 获取当前流状态
 */
inline StreamState getStreamState() {
    return g_streamState;
}

#endif // VIDEO_STREAM_H
