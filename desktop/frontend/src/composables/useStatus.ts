/**
 * 状态组合式函数
 * 统一管理系统状态的消费，供多组件共享
 *
 * 历史设计：原本通过 setInterval 每 1 秒轮询 /api/status
 * 当前设计：改为消费 useWebSocket().status（由后端 WS status 消息推送）
 *
 * 功能：
 * 1. 提供 useWebSocket().status 的便捷访问入口
 * 2. 保持向后兼容（组件无需修改 import 路径）
 *
 * 设计说明：
 * - 数据来自 WS 推送，无需引用计数启停轮询
 * - 单例由 useWebSocket 管理，本 composable 仅做透传
 */

import { useWebSocket } from './useWebSocket'
import type { StatusData } from './useWebSocket'

// 重新导出 StatusData 类型，保持向后兼容
export type { StatusData }

/**
 * 状态组合式函数
 *
 * 返回 useWebSocket 单例中的 status ref，供组件消费
 * 多组件共享同一份状态（来自 WS 推送）
 *
 * @returns status - 响应式状态数据（来自 WS 推送）
 */
export function useStatus() {
  const { status } = useWebSocket()
  return { status }
}
