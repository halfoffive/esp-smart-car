/**
 * 视频流传输系统 - 函数式编程风格
 * 基于 ESP32-S3 CAM（Freenove FNK0085），视频帧通过 WiFi UDP 分包直发到接收器
 * 支持动态质量调整和帧率控制
 * 
 * 作者：智能车项目团队
 * 版本：1.5.0（修复 P1-03 校验和统一、P2-04 发送失败中止并计丢帧）
 * 日期：2026-06-20
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
    constexpr uint8_t JPEG_QUALITY_MIN = 5;   // 最小压缩值（最高质量，驱动中数值越小质量越高）
    constexpr uint8_t JPEG_QUALITY_MAX = 50;  // 最大压缩值（最低质量，驱动中数值越大质量越低）
}

// ============================================
// 全局状态（可变）
// ============================================
inline StreamState g_streamState(false, 0, 0, 0, 0, 0);
inline uint16_t g_frameId = 0;
/// 连续帧捕获失败计数（用于错误恢复）
inline uint8_t g_consecutiveFailures = 0;
/// 全局遥测 UDP 对象，由 car_controller.ino 定义
extern WiFiUDP g_udpTelemetry;
/// 连续失败超过此阈值时重启摄像头
constexpr uint8_t CAMERA_RESTART_THRESHOLD = 10;

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
inline uint8_t adjustQuality(const uint32_t frameSize) {
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

        // 统一将校验和写入结构体 checksum 字段，并同步写入实际发送包的最后一个字节
        packet.checksum = sum;
        uint8_t* const txChecksumPtr = reinterpret_cast<uint8_t*>(&packet) + sendSize - 1;
        *txChecksumPtr = sum;

        // 通过 WiFi UDP 发送到接收器（AP 的固定 IP）
        IPAddress apIp(NetworkConfig::AP_IP[0], NetworkConfig::AP_IP[1],
                       NetworkConfig::AP_IP[2], NetworkConfig::AP_IP[3]);
        g_udpTelemetry.beginPacket(apIp, UdpConfig::TELEMETRY_PORT);
        g_udpTelemetry.write(reinterpret_cast<const uint8_t*>(&packet), sendSize);
        if (!g_udpTelemetry.endPacket()) {
            Serial.println("[UDP] 视频分包发送失败，中止该帧");
            return false;
        }

        // 短暂延迟避免拥塞
        delayMicroseconds(50);
    }

    return true;
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
 * 更新流传输状态（统一调度帧捕获+发送+质量调整）
 * 可在 loop() 中直接调用，由 car_controller.ino 选择使用
 */
inline void updateStreaming(const CameraConfiguration& config) {
    if (!g_streamState.isStreaming) return;
    
    const uint32_t currentTime = millis();
    
    // 检查帧间隔
    if (currentTime - g_streamState.lastFrameTime < VideoStreamConfig::FRAME_INTERVAL) {
        return;
    }
    
    // 捕获帧
    const FrameState frame = captureFrame();
    if (!frame.isValid) {
        g_consecutiveFailures++;
        g_streamState = StreamState(
            true, g_streamState.lastFrameTime, g_streamState.fps,
            g_streamState.totalFrames, g_streamState.droppedFrames + 1,
            g_streamState.bytesSent
        );
        // 连续失败超过阈值时重启摄像头硬件
        if (g_consecutiveFailures >= CAMERA_RESTART_THRESHOLD) {
            Serial.printf("[视频流] 连续 %d 次帧捕获失败，重启摄像头...\n",
                          g_consecutiveFailures);
            esp_camera_deinit();
            delay(500);
            // 重新初始化摄像头
            if (!initializeCamera(config)) {
                Serial.println("[视频流] 摄像头重启失败，继续重试...");
            } else {
                Serial.println("[视频流] 摄像头重启成功");
            }
            g_consecutiveFailures = 0;
        }
        return;
    }
    
    // 帧捕获成功，重置连续失败计数
    g_consecutiveFailures = 0;
    
    // 发送帧
    const bool sent = sendVideoFrame(frame);

    // 动态调整质量（根据帧大小自适应压缩率）
    const uint8_t newQuality = adjustQuality(frame.frameSize);
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_quality(sensor, newQuality);
    }

    // 更新状态：帧内任一包发送失败即计为丢帧
    const uint16_t fps = calculateFPS(g_streamState.lastFrameTime, currentTime);
    if (sent) {
        g_streamState = StreamState(
            true, currentTime, fps,
            g_streamState.totalFrames + 1, g_streamState.droppedFrames,
            g_streamState.bytesSent + frame.frameSize
        );
    } else {
        g_streamState = StreamState(
            true, currentTime, fps,
            g_streamState.totalFrames + 1, g_streamState.droppedFrames + 1,
            g_streamState.bytesSent
        );
    }
    
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
