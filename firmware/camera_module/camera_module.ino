/**
 * ESP32-S3 CAM 摄像头模块主程序
 * 基于函数式编程思想
 * 
 * 功能：
 * 1. 初始化摄像头
 * 2. 通过 SoftwareSerial 软串口传输视频帧到车载控制器
 * 3. 支持动态分辨率调整
 * 
 * 硬件：ESP32-S3 CAM + OV2640
 * 通信：SoftwareSerial (TX=GPIO14) -> ESP32-C6 车载控制器 (RX=GPIO2)
 * 注意：SoftwareSerial 在 921600 波特率下不可靠，实测如花屏需降波特率
 * 
 * 作者：智能车项目团队
 * 版本：1.4.0
 */

#include <SoftwareSerial.h>
#include "camera_config.h"
#include "video_stream.h"

// ============================================
// 软串口配置（与车载控制器通信）
// TX=GPIO14（LED 闪光灯引脚，空闲可用）
// RX=GPIO1（占位引脚，摄像头只发不收，实际未接线）
// ============================================
SoftwareSerial camSerial(1, 14);  // RX=GPIO1（占位）, TX=GPIO14

// ============================================
// 全局状态
// ============================================
CameraConfiguration g_cameraConfig = createDefaultConfig();

// ============================================
// 初始化
// ============================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n================================");
    Serial.println("ESP32-S3 CAM 视频传输模块");
    Serial.println("版本: 1.4.0 (SoftwareSerial 软串口)");
    Serial.println("================================");
    
    // PSRAM 诊断（摄像头 DMA 缓冲依赖 PSRAM）
    if (psramFound()) {
        Serial.printf("[内存] PSRAM 已启用: %.1f MB\n", ESP.getPsramSize() / 1048576.0f);
    } else {
        Serial.println("[内存] ⚠ PSRAM 未启用！请在 Arduino IDE 中设置 Tools→PSRAM→OPI PSRAM");
        Serial.println("[内存] 摄像头需要 PSRAM，初始化将失败");
    }
    Serial.println("");
    
    // 初始化摄像头
    if (!initializeCamera(g_cameraConfig)) {
        Serial.println("[摄像头] 初始化失败，重启中...");
        delay(2000);
        ESP.restart();
    }
    
    // 初始化 SoftwareSerial 软串口（与车载控制器通信）
    // TX=GPIO14 -> 车载 C6 RX=GPIO2
    camSerial.begin(921600);
    delay(100);
    Serial.println("[初始化] SoftwareSerial 初始化完成 (TX=GPIO14, 921600 baud)");
    
    // 启动视频流
    startStreaming();
    
    Serial.println("[摄像头] 视频流传输已启动");
}

// ============================================
// 主循环
// ============================================

void loop() {
    // 检查帧间隔
    const uint32_t currentTime = millis();
    if (currentTime - g_streamState.lastFrameTime < VideoStreamConfig::FRAME_INTERVAL) {
        delay(1);
        return;
    }
    
    // 捕获帧
    const FrameState frame = captureFrame();
    if (!frame.isValid) {
        // 更新丢弃计数
        g_streamState = StreamState(
            true, g_streamState.lastFrameTime, g_streamState.fps,
            g_streamState.totalFrames, g_streamState.droppedFrames + 1,
            g_streamState.bytesSent
        );
        // 连续失败恢复逻辑
        g_consecutiveFailures++;
        if (g_consecutiveFailures >= CAMERA_RESTART_THRESHOLD) {
            Serial.printf("[视频流] 连续 %d 次帧捕获失败，重启摄像头...\n",
                          g_consecutiveFailures);
            esp_camera_deinit();
            delay(500);
            if (!initializeCamera(g_cameraConfig)) {
                Serial.println("[视频流] 摄像头重启失败，继续重试...");
            } else {
                Serial.println("[视频流] 摄像头重启成功");
            }
            g_consecutiveFailures = 0;
        }
        delay(1);
        return;
    }
    
    // 帧捕获成功，重置连续失败计数
    g_consecutiveFailures = 0;
    
    // 通过 SoftwareSerial 软串口发送视频帧到车载控制器
    // 格式: [0xAA][0x55][帧大小4字节][帧数据]
    // 注意：SoftwareSerial 无 availableForWrite()，直接分块写入+flush
    const uint8_t header[] = {0xAA, 0x55};
    const uint32_t frameSize = static_cast<uint32_t>(frame.frameSize);
    
    camSerial.write(header, 2);
    camSerial.write(reinterpret_cast<const uint8_t*>(&frameSize), 4);
    
    // 分块发送帧数据（避免一次性写入大量数据导致软串口缓冲区溢出）
    constexpr size_t CHUNK_SIZE = 256;  // SoftwareSerial 缓冲小，用更小的块
    size_t sent = 0;
    while (sent < frame.frameSize) {
        const size_t chunkLen = min(CHUNK_SIZE, frame.frameSize - sent);
        camSerial.write(frame.frameBuffer->buf + sent, chunkLen);
        sent += chunkLen;
        camSerial.flush();  // 每块等待发送完成，防止丢字节
    }
    
    // 动态调整质量
    const uint8_t newQuality = adjustQuality(frame.frameSize);
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
    
    // 每100帧打印一次统计
    static uint32_t lastLoggedFrame = 0;
    const StreamState state = getStreamState();
    if (state.totalFrames != lastLoggedFrame && state.totalFrames % 100 == 0 && state.totalFrames > 0) {
        lastLoggedFrame = state.totalFrames;
        Serial.printf("[视频流] FPS:%d, 总帧:%lu, 丢弃:%lu, 发送:%lu KB\n",
                      state.fps,
                      (unsigned long)state.totalFrames,
                      (unsigned long)state.droppedFrames,
                      (unsigned long)(state.bytesSent / 1024));
    }
    
    delay(1);
}
