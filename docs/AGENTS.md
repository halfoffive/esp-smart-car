# 文档 - Knowledge Base

**Location:** `docs/`

## Structure

```
docs/
└── hardware.md          # 硬件接线说明
```

## Where to Look

| Task | Location | Notes |
|------|----------|-------|
| 查看硬件接线 | `hardware.md` | 完整引脚图 |
| 查看电源配置 | `hardware.md` | 电源分配 |
| 查看故障排除 | `hardware.md` | 常见问题 |

## Contents

- **系统架构图**：整体连接示意图
- **ESP32-C6 引脚分配**：GPIO 映射
- **L298N 接线**：电机驱动连接
- **ESP32-S3 CAM 引脚**：摄像头模块
- **电源系统**：7.4V 电池 + 稳压
- **注意事项**：共地、隔离、散热
- **调试清单**：逐项检查

## Notes

- 文档使用 Markdown 格式
- 包含代码块和表格
- 中文注释

## 近期更新

### 2026-06-21 - Karpathy 审计修复 v2 文档同步

- 同步根 `AGENTS.md` 关键变更：WebSocket owner 上移至 `App.vue`，`ControlPanel.vue` 只读使用 `useWebSocket`
- 同步前端测速面板改为 2 个模块（当前车轮速度 cm/s、轮子转速 RPM）
- 同步固件 UDP 端口分离（控制 9000 / 遥测 9001 / 视频 9002）与动态车载 IP 描述
- 同步串口状态判断改为 `startsWith('已连接')`
- 同步后端认证中间件恒定时间比较、WebSocket `Notify` 事件驱动广播、原子计数器、`build.rs` 不再自动构建前端等变更
- 同步 `README.md` 版本号与构建说明更新

### 2026-06-20 - 文档同步与 Karpathy 审计报告

- 新增 `docs/karpathy_vulnerability_report.md`，汇总 Karpathy 指南漏洞审计结果：52 项独立问题（P0×4、P1×14、P2×24、P3×10），含修复建议与验证方式
- 同步更新 `AGENTS.md`（根、`desktop/backend/`、`desktop/frontend/`）中的近期修复记录
- 修正 `docs/hardware.md` 中 `last_odom_ms` 字段描述为“距离上次收到车载数据的毫秒数”
- 修正源码中仍使用“ESP-NOW 配对”等已废弃架构描述的注释
