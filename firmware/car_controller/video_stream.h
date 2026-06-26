/**
 * 视频流传输系统 - 函数式编程风格
 * 基于 ESP32-S3 CAM（Freenove FNK0085），视频帧通过 WiFi UDP 整帧直发到接收器
 * 支持动态质量调整和帧率控制
 * 
 * 作者：智能车项目团队
 * 版本：2.1.1（QQVGA 低分辨率适配，TARGET_MIN 800→400）
 * 日期：2026-06-26
 */

#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H

#include <Arduino.h>
#include <WiFiUdp.h>
#include "esp_camera.h"
#include "../libraries/wireless_protocol/src/wireless.h"  // 复用无线通信协议（Arduino 库）

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
// 整帧传输协议常量
// ============================================
namespace FrameProtocol {
    /// 整帧传输帧头标记（4字节）
    constexpr uint8_t FRAME_HEADER[4] = {0xAA, 0x55, 0xAA, 0x55};
    /// 最大帧大小限制（MTU 安全：1400B JPEG + 6B 包头 = 1406B ≪ 1460B MTU，彻底避免 IP 分片）
    constexpr size_t MAX_FRAME_SIZE = 1400;
    /// 帧头后紧跟的帧大小字段字节数
    constexpr uint8_t SIZE_FIELD_BYTES = 2;
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
 * 目标：QQVGA 160x120 下每帧控制在 400-1400 字节（MTU 安全，无需 IP 分片）
 * 
 * 注意：ESP32 摄像头驱动中压缩值越小 = 质量越高 = 帧越大
 */
inline uint8_t adjustQuality(const uint32_t frameSize, const uint8_t currentQuality) {
    constexpr uint32_t TARGET_MAX = FrameProtocol::MAX_FRAME_SIZE; // 帧上限 = MTU 安全值（≤1400，彻底避开 IP 分片）
    constexpr uint32_t TARGET_MIN = 400;   // 帧下限（QQVGA 下约 0.4KB，保证基本画质）
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
// 传输函数（整帧单包）
// ============================================

/**
 * 发送完整视频帧（C6 端 lwIP 已启用 IP 分片重组，大帧可正常接收）
 * 将 JPEG 帧整体封装为单包 UDP 数据，直接发送到接收器
 * 帧格式：[0xAA 0x55 0xAA 0x55][帧大小(2B LE)][JPEG 数据]
 * S3 单芯片架构下由独立 FreeRTOS 任务调用
 * 返回：true 发送成功，false 发送失败
 */
inline bool sendVideoFrame(const FrameState& frame) {
    if (!frame.isValid) return false;

    const uint8_t* data = frame.frameBuffer->buf;
    const size_t totalLen = frame.frameSize;

    // 安全检查：帧超过单包上限时丢弃（调整质量后自动收敛，初启 2-3 帧正常）
    if (totalLen > FrameProtocol::MAX_FRAME_SIZE) {
        Serial.printf("[视频流] 帧超限(%u > %u)，丢弃（质量自动调整中...）\n",
                      totalLen, FrameProtocol::MAX_FRAME_SIZE);
        return false;
    }

    // 目标地址
    const IPAddress apIp(NetworkConfig::AP_IP[0], NetworkConfig::AP_IP[1],
                         NetworkConfig::AP_IP[2], NetworkConfig::AP_IP[3]);

    // 构建整帧数据：帧头 + 大小(2字节小端) + 帧数据
    // 总大小 = 4(头) + 2(大小) + frameSize
    const size_t packetSize = 4 + 2 + totalLen;
    static uint8_t packet[4 + 2 + FrameProtocol::MAX_FRAME_SIZE];  // BSS 段，不走 FreeRTOS 任务栈

    // 写入帧头
    memcpy(packet, FrameProtocol::FRAME_HEADER, 4);
    // 写入帧大小（小端）
    packet[4] = totalLen & 0xFF;
    packet[5] = (totalLen >> 8) & 0xFF;
    // 写入帧数据
    memcpy(packet + 6, data, totalLen);

    // 通过 WiFi UDP 整帧发送到接收器（使用独立 g_udpVideo 对象，避免与 g_udpTelemetry 并发竞态）
    // 首帧诊断：输出目标 IP 和包大小，便于排查 UDP 路由问题
    static bool s_firstFrameSent = false;
    if (!s_firstFrameSent) {
      Serial.printf("[UDP] 首帧发送 -> %s:%d，大小 %u 字节（MTU-安全 ≤1406B）\n",
                    apIp.toString().c_str(), UdpConfig::VIDEO_PORT, packetSize);
      s_firstFrameSent = true;
    }
    if (!g_udpVideo.beginPacket(apIp, UdpConfig::VIDEO_PORT)) {
      Serial.println("[UDP] 视频 beginPacket 失败");
      return false;
    }
    g_udpVideo.write(packet, packetSize);
    if (!g_udpVideo.endPacket()) {
        Serial.println("[UDP] 视频帧发送失败");
        return false;
    }

    return true;
}

/**
 * 启动流传输
 */
inline void startStreaming() {
    g_streamState = {};
    g_streamState.isStreaming = true;
    Serial.println("[视频流] 开始传输（整帧单包模式）");
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
