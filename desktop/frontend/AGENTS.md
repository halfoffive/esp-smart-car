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
├── tailwind.config.js     # TailwindCSS 配置
├── index.html             # 入口 HTML
├── tsconfig.json          # TypeScript 配置
└── src/
    ├── App.vue            # 根组件
    ├── main.ts            # 入口文件
    ├── style.css          # 全局样式
    ├── components/
    │   ├── VideoPlayer.vue    # 视频播放器
    │   ├── ControlPanel.vue   # 控制面板
    │   └── StatusBar.vue      # 状态栏
    └── composables/
        ├── useWebSocket.ts   # WebSocket 连接
        └── useKeyboard.ts   # 键盘控制
```

## Where to Look

| Task | Location | Notes |
|------|----------|-------|
| 修改 UI 布局 | `src/App.vue` | 主布局结构 |
| 修改视频显示 | `src/components/VideoPlayer.vue` | 实时视频 |
| 修改控制面板 | `src/components/ControlPanel.vue` | WASD + 云台 |
| 修改状态栏 | `src/components/StatusBar.vue` | 连接状态 |
| 修改 WebSocket | `src/composables/useWebSocket.ts` | 连接管理 |
| 修改键盘控制 | `src/composables/useKeyboard.ts` | 键盘映射 |
| 修改样式 | `src/style.css` | TailwindCSS 自定义 |

## Code Map

| Symbol | Type | Location | Role |
|--------|------|----------|------|
| `App` | Vue SFC | `App.vue` | 根组件 |
| `VideoPlayer` | Vue SFC | `VideoPlayer.vue` | 视频显示 |
| `ControlPanel` | Vue SFC | `ControlPanel.vue` | 控制面板 |
| `StatusBar` | Vue SFC | `StatusBar.vue` | 状态栏 |
| `useWebSocket` | Composable | `useWebSocket.ts` | WebSocket |
| `useKeyboard` | Composable | `useKeyboard.ts` | 键盘控制 |

## Conventions

- **Composition API**：使用 `<script setup>` 和组合式函数
- **Pinia**：状态管理（已配置）
- **TailwindCSS**：工具类优先，自定义主题
- **类型安全**：TypeScript 严格模式，禁止 `any`
- **响应式**：使用 `ref` 和 `computed`
- **事件处理**：键盘事件 + 鼠标事件 + WebSocket 消息

## Component API

### VideoPlayer
- **Props**: 无
- **Emits**: 无
- **State**: `videoSrc`, `fps`, `isConnected`
- **Features**: 视频显示、FPS 计算、截图、录制

### ControlPanel
- **Props**: 无
- **Emits**: `command`, `speed`
- **State**: `activeKeys`, `currentSpeed`, `logs`
- **Features**: WASD 控制、速度调节、云台控制、日志

### StatusBar
- **Props**: 无
- **Emits**: 无
- **State**: `isConnected`, `fps`, `currentSpeed`
- **Features**: 连接状态、帧率、速度显示

## Anti-Patterns

- **禁止使用 `any`**：TypeScript 严格类型
- **禁止使用 `@ts-ignore`**：类型错误必须修复
- **禁止空 catch 块**：错误必须处理或上报
- **禁止全局状态**：使用 Pinia 或组合式函数

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
- **键盘**：支持 WASD + 空格 + Q/E + U/D/L/R/C + 1-9
- **响应式**：控制面板自适应，移动端友好
- **主题**：深色模式，使用 `dark-` 颜色系列
