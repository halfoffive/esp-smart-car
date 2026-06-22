/**
 * ESP32-S3 CAM 摄像头配置
 * 基于函数式编程思想
 * 支持 OV2640 摄像头模块
 * 作者：智能车项目团队
 * 版本：1.2.0
 */

#ifndef CAMERA_CONFIG_H
#define CAMERA_CONFIG_H

#include <Arduino.h>
#include "esp_camera.h"

// ============================================
// 摄像头引脚配置（ESP32-S3 CAM 标准引脚）
// ============================================
namespace CameraPins {
    constexpr int8_t PWDN = -1;     // 电源控制（-1表示无，int8_t确保-1正确存储）
    constexpr int8_t RESET = -1;    // 复位（-1表示无，int8_t确保-1正确存储）
    constexpr uint8_t XCLK = 15;     // 外部时钟
    constexpr uint8_t SIOD = 4;      // SDA（I2C数据）
    constexpr uint8_t SIOC = 5;      // SCL（I2C时钟）
    constexpr uint8_t D7 = 16;       // 数据位7
    constexpr uint8_t D6 = 17;       // 数据位6
    constexpr uint8_t D5 = 18;       // 数据位5
    constexpr uint8_t D4 = 12;       // 数据位4
    constexpr uint8_t D3 = 10;       // 数据位3
    constexpr uint8_t D2 = 8;        // 数据位2
    constexpr uint8_t D1 = 9;        // 数据位1
    constexpr uint8_t D0 = 11;       // 数据位0
    constexpr uint8_t VSYNC = 6;     // 帧同步
    constexpr uint8_t HREF = 7;      // 行同步
    constexpr uint8_t PCLK = 13;     // 像素时钟
}

// ============================================
// 摄像头配置参数
// ============================================

/**
 * 图像分辨率枚举
 */
enum class Resolution : uint8_t {
    QQVGA = 0,    // 160x120
    QCIF = 1,     // 176x144
    HQVGA = 2,    // 240x176
    QVGA = 3,     // 320x240
    CIF = 4,      // 400x296
    VGA = 5,      // 640x480
    SVGA = 6,     // 800x600
    XGA = 7,      // 1024x768
    SXGA = 8,     // 1280x1024
    UXGA = 9      // 1600x1200
};

/**
 * 图像质量枚举
 * ESP32 摄像头驱动中，数值越小 = 质量越高（压缩越低）
 * 因此 LOW 对应最大压缩值，BEST 对应最小压缩值
 */
enum class ImageQuality : uint8_t {
    QUALITY_LOW = 35,      // 低质量（高压缩；与 JPEG_QUALITY_MAX 对齐，防像素块）
    QUALITY_MEDIUM = 25,   // 中等质量（QVGA 320x240 下 ~8-12KB/帧，10FPS 稳定）
    QUALITY_HIGH = 15,     // 高质量（低压缩）
    QUALITY_BEST = 12      // 最佳质量（最低压缩值；与 JPEG_QUALITY_MIN 对齐，防 FB-OVF）
};

/**
 * 摄像头配置结构体
 */
struct CameraConfiguration {
    Resolution resolution;      // 分辨率
    ImageQuality quality;       // 图像质量
    int8_t brightness;          // 亮度（-2到2）
    int8_t contrast;            // 对比度（-2到2）
    int8_t saturation;          // 饱和度（-2到2）
    bool verticalFlip;          // 垂直翻转
    bool horizontalMirror;      // 水平镜像

    constexpr CameraConfiguration(
        Resolution res, ImageQuality q,
        int8_t bri, int8_t con, int8_t sat,
        bool vflip, bool hmirror
    ) : resolution(res), quality(q), brightness(bri), contrast(con),
        saturation(sat), verticalFlip(vflip), horizontalMirror(hmirror) {}
};

// ============================================
// 纯函数：配置转换
// ============================================

/**
 * 纯函数：分辨率转实际像素尺寸
 */
inline std::tuple<uint16_t, uint16_t> resolutionToSize(const Resolution res) {
    switch (res) {
        case Resolution::QQVGA:  return {160, 120};
        case Resolution::QCIF:   return {176, 144};
        case Resolution::HQVGA:  return {240, 176};
        case Resolution::QVGA:   return {320, 240};
        case Resolution::CIF:    return {400, 296};
        case Resolution::VGA:    return {640, 480};
        case Resolution::SVGA:   return {800, 600};
        case Resolution::XGA:    return {1024, 768};
        case Resolution::SXGA:   return {1280, 1024};
        case Resolution::UXGA:   return {1600, 1200};
        default:                 return {320, 240};
    }
}

/**
 * 纯函数：分辨率转 esp_camera 格式
 */
inline framesize_t resolutionToFramesize(const Resolution res) {
    switch (res) {
        case Resolution::QQVGA:  return FRAMESIZE_QQVGA;
        case Resolution::QCIF:   return FRAMESIZE_QCIF;
        case Resolution::HQVGA:  return FRAMESIZE_HQVGA;
        case Resolution::QVGA:   return FRAMESIZE_QVGA;
        case Resolution::CIF:    return FRAMESIZE_CIF;
        case Resolution::VGA:    return FRAMESIZE_VGA;
        case Resolution::SVGA:   return FRAMESIZE_SVGA;
        case Resolution::XGA:    return FRAMESIZE_XGA;
        case Resolution::SXGA:   return FRAMESIZE_SXGA;
        case Resolution::UXGA:   return FRAMESIZE_UXGA;
        default:                 return FRAMESIZE_QVGA;
    }
}

/**
 * 纯函数：创建默认配置
 */
inline CameraConfiguration createDefaultConfig() {
    return CameraConfiguration(
        Resolution::QVGA,       // 320x240，4倍像素提升画质清晰度
        ImageQuality::QUALITY_MEDIUM,   // 压缩值 25（QVGA 下 ~8-12KB/帧，10FPS 稳定传输）
        0,                     // 默认亮度
        0,                     // 默认对比度
        0,                     // 默认饱和度
        false,                 // 不垂直翻转
        true                   // 水平镜像（使画面符合直觉）
    );
}

// ============================================
// 摄像头硬件配置
// ============================================

/**
 * 初始化摄像头配置
 * 副作用：配置硬件
 */
inline bool initializeCamera(const CameraConfiguration& config) {
    // 配置摄像头参数
    camera_config_t cameraConfig;
    memset(&cameraConfig, 0, sizeof(cameraConfig));
    
    cameraConfig.ledc_channel = LEDC_CHANNEL_0;
    cameraConfig.ledc_timer = LEDC_TIMER_0;
    cameraConfig.pin_pwdn = CameraPins::PWDN;
    cameraConfig.pin_reset = CameraPins::RESET;
    cameraConfig.pin_xclk = CameraPins::XCLK;
    cameraConfig.pin_sccb_sda = CameraPins::SIOD;
    cameraConfig.pin_sccb_scl = CameraPins::SIOC;
    cameraConfig.pin_d7 = CameraPins::D7;
    cameraConfig.pin_d6 = CameraPins::D6;
    cameraConfig.pin_d5 = CameraPins::D5;
    cameraConfig.pin_d4 = CameraPins::D4;
    cameraConfig.pin_d3 = CameraPins::D3;
    cameraConfig.pin_d2 = CameraPins::D2;
    cameraConfig.pin_d1 = CameraPins::D1;
    cameraConfig.pin_d0 = CameraPins::D0;
    cameraConfig.pin_vsync = CameraPins::VSYNC;
    cameraConfig.pin_href = CameraPins::HREF;
    cameraConfig.pin_pclk = CameraPins::PCLK;
    
    // 设置像素格式和分辨率
    cameraConfig.pixel_format = PIXFORMAT_JPEG;  // JPEG格式，适合传输
    cameraConfig.frame_size = resolutionToFramesize(config.resolution);
    cameraConfig.jpeg_quality = static_cast<int>(config.quality);
    cameraConfig.fb_count = 2;  // 双缓冲（避免摄像头 DMA 覆盖正在发送的帧 → FB-OVF 花屏）
    cameraConfig.xclk_freq_hz = 20000000;  // 20MHz XCLK（Freenove FNK0085 必须值；10MHz 导致摄像头驱动 DMA/中断野指针→StoreProhibited）
    cameraConfig.fb_location = CAMERA_FB_IN_PSRAM;  // 帧缓冲使用 PSRAM（S3 CAM 必需）
    
    // 初始化摄像头
    esp_err_t err = esp_camera_init(&cameraConfig);
    if (err != ESP_OK) {
        Serial.printf("[摄像头] 初始化失败: 0x%x\n", err);
        Serial.println("[摄像头] 请检查: 1)PSRAM是否开启(Tools→PSRAM→OPI) 2)摄像头排线");
        return false;
    }
    
    // 获取摄像头传感器
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        Serial.println("[摄像头] 获取传感器失败");
        return false;
    }
    
    // 应用图像参数
    sensor->set_brightness(sensor, config.brightness);
    sensor->set_contrast(sensor, config.contrast);
    sensor->set_saturation(sensor, config.saturation);
    sensor->set_vflip(sensor, config.verticalFlip);
    sensor->set_hmirror(sensor, config.horizontalMirror);
    
    const auto [width, height] = resolutionToSize(config.resolution);
    Serial.printf("[摄像头] 初始化成功: %dx%d, 质量:%d\n", 
                  width, height, static_cast<int>(config.quality));
    
    return true;
}

#endif // CAMERA_CONFIG_H
