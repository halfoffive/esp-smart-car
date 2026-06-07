# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-06-07

### Added
- **智能车嵌入式控制系统** - 基于 ESP32-C6 的完整车载控制固件
  - 函数式编程风格电机控制（`motor_control.h`）
  - L298N 双电机驱动支持（4个直流电机控制）
  - 差速转向实现（左转/右转/原地旋转）
  - WASD 命令解析系统
  - 速度分级控制（1-9级）
  - 自动超时保护（1秒无命令自动停止）
  - 紧急停止功能
  
- **舵机云台控制系统** - SG90 双舵机控制（`servo_control.h`）
  - 水平舵机控制（左右旋转）
  - 垂直舵机控制（上下旋转）
  - 平滑移动算法（逐步逼近目标角度）
  - 角度安全限制（0-180度）
  - 云台居中功能
  - 云台控制命令解析（U/D/L/R/C）
  
- **ESP-NOW 无线通信协议**（`wireless.h`）
  - 自定义二进制通信协议
  - 数据包格式：魔术字 + 版本 + 类型 + 数据 + 速度 + 序列号 + 校验和
  - 支持命令类型：MOVE / SERVO / SPEED / STOP / STATUS
  - 设备角色定义：CAR / RECEIVER / CAMERA
  - 自动重连机制
  
- **ESP32-S3 CAM 视频传输系统**（`camera_module.ino`）
  - OV2640 摄像头初始化与配置
  - 多分辨率支持（QQVGA 到 UXGA）
  - 动态 JPEG 质量调整
  - 视频帧分包传输（每包128字节）
  - 帧率控制（目标 30 FPS）
  - 帧统计与丢帧检测
  
- **电脑端接收器固件**（`receiver_dongle.ino`）
  - ESP32-C6 USB 串口桥接
  - 高速串口通信（921600 波特率）
  - 视频帧转发（帧头 0xAA 0x55 + 帧大小 + 帧数据）
  - 命令路由（串口命令 → ESP-NOW 转发）
  - 心跳包机制
  
- **Rust 桌面后端**（`desktop/backend/`）
  - Axum Web 框架 HTTP 服务器
  - WebSocket 实时通信（视频传输 + 命令控制）
  - 串口通信管理（`serialport` 库）
  - 视频帧 Base64 编码
  - 心跳保活机制
  - RESTful API（命令发送 / 状态查询 / 串口连接）
  - 多客户端 WebSocket 管理
  - CORS 跨域支持
  - 静态文件服务（前端构建产物）
  
- **Vue 前端 Web UI**（`desktop/frontend/`）
  - Vue 3 + Vite + TypeScript 技术栈
  - TailwindCSS 深色主题界面
  - 实时视频播放器（WebSocket 接收 Base64 JPEG 帧）
  - 完整控制面板
    - WASD 键盘控制（支持物理按键映射）
    - 鼠标点击控制（按钮式）
    - 速度滑块控制（1-9级）
    - 云台方向控制（U/D/L/R/C）
    - 紧急停止按钮
  - 状态栏显示
    - WebSocket 连接状态
    - 串口连接状态
    - 实时帧率（FPS）
    - 当前速度
    - 接收帧数
  - 系统日志面板
  - 截图功能（下载当前视频帧）
  - 录制功能（界面预留）
  
- **项目文档**
  - `README.md` - 完整项目说明（架构、安装、使用）
  - `docs/hardware.md` - 详细硬件接线图
  - `.gitignore` - Git 忽略配置
  - `.env` - 后端环境配置

### Technical Details

#### 嵌入式固件特点
- **函数式编程风格**：大量 const 数据、纯函数、显式状态传递
- **中文注释**：所有函数、结构体、枚举均有详细中文注释
- **模块化设计**：电机、舵机、无线通信完全分离
- **类型安全**：使用 enum class 替代 enum，避免隐式转换
- **防错误机制**：电机超时保护、舵机角度限制、数据包校验

#### 通信协议
- 控制命令：单字节字符（W/A/S/D/Q/E/空格/1-9/U/D/L/R/C）
- 数据包结构：8字节固定长度 + 校验和
- 视频帧格式：帧头(2) + 大小(4) + 数据(N)
- 传输方式：ESP-NOW 无线 + USB 串口

#### 前端架构
- **状态管理**：Pinia（Vue 官方推荐）
- **组合式函数**：useWebSocket（WebSocket 连接管理）、useKeyboard（键盘事件）
- **响应式设计**：TailwindCSS 工具类，支持暗色主题
- **性能优化**：requestAnimationFrame 视频帧更新

### Hardware Requirements
- ESP32-C6 开发板 × 2（车载 + 接收器）
- ESP32-S3 CAM × 2（摄像头模块）
- L298N 电机驱动模块 × 2
- 直流减速电机 × 4
- SG90 舵机 × 2
- 7.4V 锂电池
- 5V 稳压模块

### Dependencies

#### 嵌入式（Arduino IDE）
- ESP32 Board Package（支持 C6 和 S3）
- ESP32Camera 库
- ESP-NOW 协议（内置）

#### 后端（Rust）
- axum 0.7（Web 框架）
- tokio 1（异步运行时）
- serialport 4.3（串口通信）
- serde 1.0（序列化）
- base64 0.22（编码）
- chrono 0.4（时间处理）
- tracing 0.1（日志）
- anyhow 1.0（错误处理）
- dotenvy 0.15（环境变量）

#### 前端（Node.js/Bun）
- vue 3.4.21
- vite 5.2.8
- tailwindcss 3.4.3
- typescript 5.4.5
- pinia 2.1.7

### Known Issues
- 视频传输在高分辨率下可能卡顿（建议降低分辨率或帧率）
- ESP-NOW 通信距离受环境影响（建议空旷场地使用）
- 舵机电源需要独立供电（避免干扰 ESP32）

### Future Plans
- 增加第二路摄像头支持
- 添加传感器数据（超声波、红外）
- 实现路径记录与回放功能
- 添加自动避障算法
- 支持手机端控制界面
- 添加电池电量监测

---

[Unreleased]: https://github.com/yourusername/esp-smart-car/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/yourusername/esp-smart-car/releases/tag/v1.0.0
