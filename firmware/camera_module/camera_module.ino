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
 * 版本：1.2.0
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
            // 云台命令由接收器转发到车载控制器
            // 摄像头仅与接收器配对，sendToCar(CAR_MAC) 会因 CAR_MAC 不在 peer 表中而静默失败
            break;
        case CommandType::STATUS:
            // 状态查询：记录当前视频流状态到串口日志（ESP-NOW 状态回复尚未实现）
            {
                const StreamState state = getStreamState();
                Serial.printf("[状态] 流传输:%s, FPS:%d, 总帧:%lu\n",
                              state.isStreaming ? "ON" : "OFF",
                              state.fps,
                              (unsigned long)state.totalFrames);
            }
            break;
        default:
            break;
    }
}

// ============================================
// ESP-NOW 回调
// ============================================

void onCameraDataRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
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
    Serial.println("版本: 1.2.0");
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
    
    if (esp_now_register_recv_cb(onCameraDataRecv) != ESP_OK) {
        Serial.println("[无线通信] 注册接收回调失败");
    }
    
    // 启动视频流
    startStreaming();
    
    Serial.println("[摄像头] 视频流传输已启动");
}

// ============================================
// 主循环
// ============================================

void loop() {
    // 更新视频流传输
    updateStreaming(g_cameraConfig);
    
    // 每100帧打印一次统计（使用 lastLoggedFrame 防止同一帧重复打印）
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
    
    // 小延迟
    delay(1);
}
