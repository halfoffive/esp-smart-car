/**
 * 视频流传输系统 - 函数式编程风格
 * 基于 ESP32-S3 CAM，通过 ESP-NOW 传输 JPEG 帧
 * 支持动态质量调整和帧率控制
 * 作者：智能车项目团队
 * 版本：1.2.0
 */

#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H

#include <Arduino.h>
#include "esp_camera.h"
#include <../libraries/wireless_protocol/src/wireless.h>  // 复用无线通信协议（Arduino 库）

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
 * 传输状态
 */
struct StreamState {
    bool isStreaming;         // 是否正在流传输
    uint32_t lastFrameTime;   // 最后帧时间
    uint16_t fps;             // 实际帧率
    uint32_t totalFrames;     // 总帧数
    uint32_t droppedFrames;   // 丢弃帧数
    uint32_t bytesSent;       // 发送字节数
    
    constexpr StreamState(
        bool stream, uint32_t last, uint16_t fps,
        uint32_t total, uint32_t drop, uint32_t bytes
    ) : isStreaming(stream), lastFrameTime(last), fps(fps),
        totalFrames(total), droppedFrames(drop), bytesSent(bytes) {}
};

// ============================================
// 常量定义
// ============================================
namespace VideoStreamConfig {
    constexpr uint16_t TARGET_FPS = 30;       // 目标帧率
    constexpr uint32_t FRAME_INTERVAL = 1000 / TARGET_FPS; // 帧间隔
    constexpr uint8_t JPEG_QUALITY_MIN = 5;   // 最小JPEG质量
    constexpr uint8_t JPEG_QUALITY_MAX = 50;  // 最大JPEG质量
}

// ============================================
// 全局状态（可变）
// ============================================
static StreamState g_streamState(false, 0, 0, 0, 0, 0);
static uint16_t g_frameId = 0;

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
 * 纯函数：计算帧率
 */
inline uint16_t calculateFPS(const uint32_t lastFrameTime, const uint32_t currentTime) {
    if (lastFrameTime == 0) return 0;
    const uint32_t diff = currentTime - lastFrameTime;
    if (diff == 0) return 0;
    return static_cast<uint16_t>(1000 / diff);
}

/**
 * 纯函数：动态调整质量
 * 根据网络状况调整JPEG质量
 */
inline uint8_t adjustQuality(const uint32_t bytesSent, const uint32_t frameSize) {
    // 如果帧太大，提高压缩率（降低质量）
    if (frameSize > 10000) {
        return VideoStreamConfig::JPEG_QUALITY_MAX;
    }
    // 如果帧很小，降低压缩率（提高质量）
    if (frameSize < 5000) {
        return VideoStreamConfig::JPEG_QUALITY_MIN;
    }
    return 20; // 默认质量
}

// ============================================
// 传输函数
// ============================================

/**
 * 发送视频帧
 * 将大帧分割为多个小包传输
 */
inline void sendVideoFrame(const FrameState& frame) {
    if (!frame.isValid) return;
    
    const uint8_t* data = frame.frameBuffer->buf;
    const size_t totalLen = frame.frameSize;
    const uint16_t totalPackets = (totalLen + StreamConfig::MAX_PACKET_SIZE - 1) / 
                                   StreamConfig::MAX_PACKET_SIZE;
    
    // 发送帧头信息
    for (uint16_t i = 0; i < totalPackets; i++) {
        const size_t offset = i * StreamConfig::MAX_PACKET_SIZE;
        const uint16_t packetLen = min(
            static_cast<size_t>(StreamConfig::MAX_PACKET_SIZE),
            totalLen - offset
        );
        
        // 构建视频包
        VideoPacket packet;
        packet.magic = StreamConfig::VIDEO_MAGIC;
        packet.version = StreamConfig::PROTOCOL_VERSION;
        packet.frameId = frame.frameId;
        packet.packetId = i;
        packet.totalPackets = totalPackets;
        packet.dataLen = packetLen;
        memcpy(packet.data, data + offset, packetLen);

        // 计算实际发送大小（不含 data 数组的未使用尾部）
        const size_t sendSize = sizeof(VideoPacket) - StreamConfig::MAX_PACKET_SIZE + packetLen;

        // 计算校验和：仅覆盖实际发送的字节（不含 checksum 字段本身）
        uint8_t sum = 0;
        const uint8_t* packetData = reinterpret_cast<const uint8_t*>(&packet);
        for (size_t j = 0; j < sendSize - 1; j++) {  // -1 排除 checksum 字段
            sum += packetData[j];
        }
        packet.checksum = sum;

        // 发送到接收器（指定 MAC 地址，避免广播给车载端造成误解析）
        sendRawPacket(WirelessConfig::RECEIVER_MAC,
                     reinterpret_cast<const uint8_t*>(&packet),
                     sendSize);
        
        // 短暂延迟避免拥塞
        delayMicroseconds(50);
    }
}

/**
 * 启动流传输
 */
inline void startStreaming() {
    g_streamState = StreamState(true, 0, 0, 0, 0, 0);
    Serial.println("[视频流] 开始传输");
}

/**
 * 停止流传输
 */
inline void stopStreaming() {
    g_streamState = StreamState(false, g_streamState.lastFrameTime, 
                                g_streamState.fps, g_streamState.totalFrames,
                                g_streamState.droppedFrames, g_streamState.bytesSent);
    Serial.println("[视频流] 停止传输");
}

/**
 * 更新流传输状态
 */
inline void updateStreaming() {
    if (!g_streamState.isStreaming) return;
    
    const uint32_t currentTime = millis();
    
    // 检查帧间隔
    if (currentTime - g_streamState.lastFrameTime < VideoStreamConfig::FRAME_INTERVAL) {
        return;
    }
    
    // 捕获帧
    const FrameState frame = captureFrame();
    if (!frame.isValid) {
        g_streamState = StreamState(
            true, g_streamState.lastFrameTime, g_streamState.fps,
            g_streamState.totalFrames, g_streamState.droppedFrames + 1,
            g_streamState.bytesSent
        );
        return;
    }
    
    // 发送帧
    sendVideoFrame(frame);
    
    // 动态调整质量（根据帧大小自适应压缩率）
    const uint8_t newQuality = adjustQuality(g_streamState.bytesSent, frame.frameSize);
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_quality(sensor, newQuality);
    }
    
    // 更新状态
    const uint16_t fps = calculateFPS(g_streamState.lastFrameTime, currentTime);
    g_streamState = StreamState(
        true, currentTime, fps,
        g_streamState.totalFrames + 1, g_streamState.droppedFrames,
        g_streamState.bytesSent + frame.frameSize
    );
    
    // 释放帧
    releaseFrame(frame);
}

/**
 * 获取当前流状态
 */
inline StreamState getStreamState() {
    return g_streamState;
}

#endif // VIDEO_STREAM_H
