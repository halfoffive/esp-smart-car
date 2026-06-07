/**
 * ESP32-S3 CAM 摄像头模块主程序
 * 基于函数式编程思想
 * 
 * 功能：
 * 1. 初始化摄像头
 * 2. 通过 ESP-NOW 传输视频帧
 * 3. 接收云台控制命令
 * 4. 支持动态分辨率调整
 * 
 * 硬件：ESP32-S3 CAM + OV2640
 * 
 * 作者：智能车项目团队
 * 版本：1.0.0
 */

#include "camera_config.h"
#include "video_stream.h"

// ============================================
// 全局状态
// ============================================
CameraConfiguration g_cameraConfig = createDefaultConfig();

// ============================================
// 命令处理
// ============================================

void handleCameraCommand(const uint8_t* data, int len) {
    if (len < sizeof(WirelessPacket)) return;
    
    const WirelessPacket* packet = reinterpret_cast<const WirelessPacket*>(data);
    if (!validatePacket(*packet)) return;
    
    switch (packet->type) {
        case CommandType::SERVO:
            // 转发舵机命令到车载控制器
            break;
        case CommandType::STATUS:
            // 发送状态反馈
            break;
        default:
            break;
    }
}

// ============================================
// ESP-NOW 回调
// ============================================

void onCameraDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    handleCameraCommand(data, len);
}

// ============================================
// 初始化
// ============================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n================================");
    Serial.println("ESP32-S3 CAM 视频传输模块");
    Serial.println("版本: 1.0.0");
    Serial.println("================================\n");
    
    // 初始化摄像头
    if (!initializeCamera(g_cameraConfig)) {
        Serial.println("[摄像头] 初始化失败，重启中...");
        delay(2000);
        ESP.restart();
    }
    
    // 初始化无线通信
    if (!initializeWireless(DeviceRole::CAMERA)) {
        Serial.println("[无线通信] 初始化失败，重启中...");
        delay(2000);
        ESP.restart();
    }
    
    esp_now_register_recv_cb(onCameraDataRecv);
    
    // 启动视频流
    startStreaming();
    
    Serial.println("[摄像头] 视频流传输已启动");
}

// ============================================
// 主循环
// ============================================

void loop() {
    // 更新视频流传输
    updateStreaming();
    
    // 每100帧打印一次统计
    const StreamState state = getStreamState();
    if (state.totalFrames % 100 == 0 && state.totalFrames > 0) {
        Serial.printf("[视频流] FPS:%d, 总帧:%d, 丢弃:%d, 发送:%d KB\n",
                      state.fps, state.totalFrames, state.droppedFrames,
                      state.bytesSent / 1024);
    }
    
    // 小延迟
    delay(1);
}
