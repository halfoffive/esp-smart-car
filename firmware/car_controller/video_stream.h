/**
 * 视频流传输系统 - 函数式编程风格
 * 基于 ESP32-S3 CAM（Freenove FNK0085），视频帧通过 WiFi UDP 分包直发到接收器
 * 支持动态质量调整和帧率控制
 * 
 * 作者：智能车项目团队
 * 版本：1.6.0（同步 Task 6：合并 StreamConfig、端口分离、滑动窗口 FPS、可变 StreamState）
 * 日期：2026-06-21
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
/// 全局遥测 UDP 对象，由 car_controller.ino 定义
extern WiFiUDP g_udpTelemetry;
/// 连续失败超过此阈值时重启摄像头
constexpr uint8_t CAMERA_RESTART_THRESHOLD = 10;
/// 当前 JPEG 压缩值（供 adjustQuality 渐进调整，初始与 camera_config.h 默认值对齐）
inline uint8_t g_currentQuality = 22;

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
 * 目标：160x120 下每帧控制在 2500-5000 字节，12 FPS 稳定传输
 * 
 * 注意：ESP32 摄像头驱动中压缩值越小 = 质量越高 = 帧越大
 */
inline uint8_t adjustQuality(const uint32_t frameSize, const uint8_t currentQuality) {
    constexpr uint32_t TARGET_MAX = 5000;  // 帧过大则加压（提高压缩值，缩小帧）
    constexpr uint32_t TARGET_MIN = 2500;  // 帧过小则减压（降低压缩值，提升质量）
    constexpr uint8_t STEP = 2;             // 每步调整量（渐进，防止剧烈振荡）

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
// 传输函数
// ============================================

/**
 * 发送视频帧
 * 将大帧分割为多个小包通过 WiFi UDP 传输到接收器
 * S3 单芯片架构下由 car_controller.ino 的 loop() 直接调用
 * 返回：true 发送成功，false 发送过程中失败（已中止）
 */
inline bool sendVideoFrame(const FrameState& frame) {
    if (!frame.isValid) return false;

    const uint8_t* data = frame.frameBuffer->buf;
    const size_t totalLen = frame.frameSize;
    const uint16_t totalPackets = (totalLen + StreamConfig::MAX_PACKET_SIZE - 1) /
                                   StreamConfig::MAX_PACKET_SIZE;

    // 目标地址移出循环，避免每包重复构造
    const IPAddress apIp(NetworkConfig::AP_IP[0], NetworkConfig::AP_IP[1],
                         NetworkConfig::AP_IP[2], NetworkConfig::AP_IP[3]);

    for (uint16_t i = 0; i < totalPackets; i++) {
        const size_t offset = i * StreamConfig::MAX_PACKET_SIZE;
        const uint16_t packetLen = min(
            static_cast<size_t>(StreamConfig::MAX_PACKET_SIZE),
            totalLen - offset
        );

        // 构建视频包
        VideoPacket packet = {};
        packet.magic = StreamConfig::VIDEO_MAGIC;
        packet.version = StreamConfig::PROTOCOL_VERSION;
        packet.frameId = frame.frameId;
        packet.packetId = i;
        packet.totalPackets = totalPackets;
        packet.dataLen = packetLen;
        memcpy(packet.data, data + offset, packetLen);

        // 计算实际发送大小：10字节头部 + packetLen字节数据 + 1字节校验和
        const size_t sendSize = 10 + packetLen + 1;  // header(10) + data + checksum(1)

        // 计算校验和：覆盖发送范围内除校验和字节外的所有字节（0 到 sendSize-2）
        uint8_t sum = 0;
        const uint8_t* packetData = reinterpret_cast<const uint8_t*>(&packet);
        for (size_t j = 0; j < sendSize - 1; j++) {
            sum += packetData[j];
        }

        // 仅通过实际发送包的最后一个字节写入校验和，避免 packed 结构体字段对齐带来的偏移误差
        uint8_t* const txChecksumPtr = reinterpret_cast<uint8_t*>(&packet) + sendSize - 1;
        *txChecksumPtr = sum;

        // 通过 WiFi UDP 发送到接收器（AP 的固定 IP），使用独立视频端口
        g_udpTelemetry.beginPacket(apIp, UdpConfig::VIDEO_PORT);
        g_udpTelemetry.write(reinterpret_cast<const uint8_t*>(&packet), sendSize);
        if (!g_udpTelemetry.endPacket()) {
            Serial.println("[UDP] 视频分包发送失败，中止该帧");
            return false;
        }

        // 本地 AP 内网传输，不需要额外延迟；移除以最大化发包速率
        // delayMicroseconds(50);
    }

    return true;
}

/**
 * 启动流传输
 */
inline void startStreaming() {
    g_streamState = {};
    g_streamState.isStreaming = true;
    Serial.println("[视频流] 开始传输");
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
