/**
 * ESP32-C6 接收器 lwIP 编译选项
 * 
 * 启用 IP 分片重组，使 C6 能接收超过 WiFi MTU（1460 字节）的 UDP 整帧视频包。
 * S3 发送的 QVGA JPEG 整帧可达 5KB，需 IP 层分片传输，C6 重组后再交付 UDP 层。
 * 
 * IP_REASS_MAX_PBUFS=20: 足够重组 ~30KB 的帧（每 pbuf 约 1500B）
 * IP_REASS_MAXAGE=30:   分片超时 30 秒（默认为 15 秒，WiFi 环境适当放宽）
 */
-DIP_REASSEMBLY=1
-DIP_REASS_MAX_PBUFS=20
-DIP_REASS_MAXAGE=30
