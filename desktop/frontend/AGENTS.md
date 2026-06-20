# 桌面端前端 - Knowledge Base

**Location:** `desktop/frontend/`
**Language:** TypeScript + Vue 3
**Framework:** Vite + Vue + TailwindCSS
**Package Manager:** Bun

## Structure

```
desktop/frontend/
├── package.json           # 依赖配置
├── vite.config.ts         # Vite 配置
├── index.html             # 入口 HTML
├── tsconfig.json          # TypeScript 配置
└── src/
    ├── App.vue            # 根组件（全屏自适应布局）
    ├── main.ts            # 入口文件
    ├── style.css          # 全局样式（含速度滑块样式）
    ├── components/
    │   ├── VideoPlayer.vue    # 视频播放器
    │   ├── ControlPanel.vue   # 控制面板（含直线修正开关）
    │   ├── StatusBar.vue      # 状态栏
    │   └── SpeedDashboard.vue # 测速仪表盘（4模块）
    └── composables/
        ├── useWebSocket.ts   # WebSocket + 测速数据
        ├── useKeyboard.ts   # 键盘控制
        ├── useApi.ts        # HTTP API 封装
        └── useStatus.ts     # 状态轮询（共享数据源）
```

## Where to Look

| Task | Location | Notes |
|------|----------|-------|
| 修改 UI 布局 | `src/App.vue` | 全屏自适应布局 |
| 修改视频显示 | `src/components/VideoPlayer.vue` | 实时视频 |
| 修改控制面板 | `src/components/ControlPanel.vue` | WASD + 速度调节 + 三态行走模式 + BLE 扫描 |
| 修改状态栏 | `src/components/StatusBar.vue` | 连接状态 |
| 修改测速显示 | `src/components/SpeedDashboard.vue` | 4模块：当前/最高/平均速度+运行信息 |
| 修改 WebSocket | `src/composables/useWebSocket.ts` | 连接管理+测速数据解析 |
| 修改键盘控制 | `src/composables/useKeyboard.ts` | 键盘映射 |
| 修改样式 | `src/style.css` | TailwindCSS 自定义 |

## Code Map

| Symbol | Type | Location | Role |
|--------|------|----------|------|
| `App` | Vue SFC | `App.vue` | 根组件 |
| `VideoPlayer` | Vue SFC | `VideoPlayer.vue` | 视频显示 |
| `ControlPanel` | Vue SFC | `ControlPanel.vue` | 控制面板+直线修正 |
| `StatusBar` | Vue SFC | `StatusBar.vue` | 状态栏 |
| `SpeedDashboard` | Vue SFC | `SpeedDashboard.vue` | 测速仪表盘 |
| `useWebSocket` | Composable | `useWebSocket.ts` | WebSocket+测速 |
| `useKeyboard` | Composable | `useKeyboard.ts` | 键盘控制 |
| `useApi` | Composable | `useApi.ts` | HTTP API 调用封装 |
| `useStatus` | Composable | `useStatus.ts` | 状态轮询管理 |
| `OdometryData` | Interface | `useWebSocket.ts` | 测速数据类型 |

## Conventions

- **Composition API**：使用 `<script setup>` 和组合式函数
- **状态管理**：组合式函数（composables）单例模式，无需 Pinia 等外部状态库
- **TailwindCSS**：工具类优先，自定义主题
- **类型安全**：TypeScript 严格模式，禁止 `any`
- **响应式**：使用 `ref` 和 `computed`
- **事件处理**：键盘事件 + 鼠标事件 + WebSocket 消息

## Component API

### VideoPlayer
- **Props**: 无
- **Emits**: 无
- **State**: `videoSrc`, `fps`, `isConnected`
- **Features**: 视频显示、FPS 计算、截图

### ControlPanel
- **Props**: 无
- **Emits**: `command`, `speed`
- **State**: `activeKeys`, `currentSpeed` (0-255 PWM), `logs`, `driveMode`
- **Features**: WASD 控制、速度调节（0-255 PWM）、三态行走模式（普通/直线/锁定）、BLE 扫描、系统日志

### SpeedDashboard
- **Props**: 无（从 useWebSocket 获取测速数据）
- **State**: `maxLeftSpeed`, `maxRightSpeed`, `commandCount`, `runTimeSeconds`
- **Features**: 4个测速模块、实时速度显示、最高速度记录、平均速度、航向角、运行时长

### StatusBar
- **Props**: 无
- **State**: `isConnected`, `serialConnected`, `fps`, `currentSpeed` (0-255 PWM), `frameCount`
- **Features**: 连接状态、帧率、速度显示（0-255 PWM）

## Anti-Patterns

- **禁止使用 `any` 或 `@ts-ignore`**：类型安全是强制性的
- **禁止空 catch 块**：错误必须被处理或报告
- **禁止删除失败的测试**：修复代码，而不是测试
- **禁止全局可变状态**：使用组合式函数单例模式管理状态
- **禁止全屏滚动**：UI 必须适应 100vh，无需滚动

## Commands

```bash
# 安装依赖
bun install

# 开发
bun run dev          # 启动开发服务器（端口 3000）

# 构建
bun run build        # 生产构建（输出到 dist/）

# 预览
bun run preview      # 预览生产构建
```

## Notes

- **代理**：Vite 配置代理 `/api` 和 `/ws` 到后端（端口 8080）
- **视频帧**：WebSocket 接收 Base64 JPEG，显示为 `data:image/jpeg;base64,...`
- **测速数据**：WebSocket 接收 `odometry` 类型消息，包含左右轮速度、航向、距离
- **键盘**：支持 WASD + 空格 + Q/E；数字键 1-9 作为 0-255 PWM 快捷档位
- **速度语义**：WebSocket `speed` 消息与 `StatusData.currentSpeed` 均为 0-255 PWM
- **智能修正**：通过 WebSocket 发送 `drive_mode` 命令切换
- **响应式**：全屏100vh布局，右侧面板含控制+测速模块
- **主题**：深色模式，使用 `dark-` 颜色系列
- **WebSocket 单管理员模式**：`useWebSocket(owner = false)` — 只有 `owner=true` 的调用者（App.vue）才能执行 `connect()`/`disconnect()`，其他组件只消费状态。防止多组件卸载时意外断开全局连接。
- **重连保护**：`disconnect()` 设置 `shouldReconnect = false` 后再关闭 socket，阻止 `onclose` handler 自动重连。

## 近期修复记录

### 2026-06-20 - Karpathy 审计修复

**背景**: 完成 Karpathy 指南漏洞审计，报告见 `docs/karpathy_vulnerability_report.md`，共发现并修复 52 项问题。

**本模块修复**:

- **P1**:
  - WebSocket 重连指数退避修复 — `useWebSocket.ts` 区分手动连接与自动重连，自动重连时正确累加 `retryCount`
  - 串口状态判断 — `ControlPanel.vue` / `StatusBar.vue` 将 `serialStatus === '已连接'` 改为 `startsWith('已连接')`，与后端 WS 推送格式对齐
- **P2**:
  - 连接/断开 50ms 竞态 — `useWebSocket.ts` 引入连接尝试 generation 计数器，避免 `disconnect()` 与延迟中的 `connect()` 状态不一致
  - 行走模式状态同步 — `ControlPanel.vue` + 后端 `status` 消息增加 `drive_mode` 字段，UI 以服务端状态为准
  - `JSON.parse` 类型安全 — `useWebSocket.ts` `socket.onmessage` 中 `JSON.parse` 返回 `unknown`，并增加 `typeof message === 'object'` 守卫
  - BLE `wifi_mac` 类型守卫 — `useWebSocket.ts` 映射 `ble_devices` 时显式校验 `wifi_mac` 类型
  - 剪贴板写入错误处理 — `ControlPanel.vue` `selectBleDevice` 复制失败时记录 warning 日志
  - 串口回滚错误处理 — `ControlPanel.vue` `connect()` WebSocket 连接失败后回滚串口时，将错误记录到日志
  - 串口扫描空结果清理 — `ControlPanel.vue` `scanPorts()` 后端返回空列表或失败时清空 `scannedPorts`
  - MAC 链接虚假成功 — `ControlPanel.vue` `linkManualMac` 增加客户端 MAC 格式校验
- **P3**:
  - 视频截图错误处理 — `VideoPlayer.vue` `takeSnapshot` 临时追加 `<a>` 到 DOM，并捕获下载错误
  - 版本号一致性 — `package.json` 与 `App.vue` 统一版本号来源
  - 注释清理 — 修正源码中 `ESP-NOW` 等遗留描述

**验证**: `bun run build` 通过；`vue-tsc --noEmit` 无类型错误。