/**
 * Wi-Fi 凭据配置模板
 *
 * 使用方式：
 * 复制本文件为 wifi_credentials.h，并修改 SSID/PASSWORD。
 * wifi_credentials.h 已加入 .gitignore，不会进入版本库。
 */

#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

// 软接入点 SSID（建议包含设备标识以避免多车冲突）
#define WIFI_AP_SSID "ESP-SmartCar"

// 软接入点密码：必须为 8-63 字节 ASCII，且每台设备唯一
#define WIFI_AP_PASSWORD "ChangeMeToUniqueStrongPassword"

#endif // WIFI_CREDENTIALS_H
