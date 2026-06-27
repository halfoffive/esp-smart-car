/**
 * 后端健康检测组合式函数
 * 自动探测后端是否可用，不可用时禁用前端控制 UI
 *
 * 功能：
 * 1. 启动时探测 /api/status（1 秒超时）
 * 2. 每 10 秒重试一次（后端恢复时自动标记可用）
 * 3. 暴露响应式 backendAvailable 状态
 * 4. 提供 recheck() 方法手动触发检测
 *
 * 设计模式：闭包 + 单例模式（与 useWebSocket.ts 风格一致）
 * - 所有状态封装在工厂函数闭包中，避免模块级全局变量（HMR 友好）
 * - 单例实例在首次调用时创建，整个应用生命周期共享
 * - HMR 重载时清理旧定时器，避免定时器泄漏
 */

import { ref } from 'vue'
import type { Ref } from 'vue'
import { DEFAULT_API_TOKEN } from '../config/auth'

/** 探测间隔（毫秒）：每 10 秒重试一次 */
const CHECK_INTERVAL = 10000
/** 探测超时（毫秒）：1 秒内无响应视为后端不可用 */
const PROBE_TIMEOUT = 1000

/** 后端健康检测实例接口 */
interface BackendHealthInstance {
  backendAvailable: Ref<boolean>
  recheck: () => void
  start: () => void
  stop: () => void
}

/**
 * 创建后端健康检测实例（工厂函数）
 * 所有状态封装在闭包内，避免模块级全局变量污染
 */
function createBackendHealth(): BackendHealthInstance {
  // 初始假设后端可用，避免启动时短暂闪烁红色横幅
  // 首次探测（立即执行）会在 1 秒内更新为真实状态
  const backendAvailable = ref(true)

  // 防止并发探测：上一次探测未完成时跳过
  let isChecking = false
  // 保存定时器 ID，HMR 或组件卸载时清理
  let intervalId: ReturnType<typeof setInterval> | null = null

  /**
   * 探测后端：fetch /api/status，1 秒超时
   * - HTTP 2xx 响应：标记后端可用
   * - 网络错误/超时/非 2xx：标记后端不可用
   */
  const check = async () => {
    if (isChecking) return
    isChecking = true
    const controller = new AbortController()
    const timeoutId = setTimeout(() => controller.abort(), PROBE_TIMEOUT)
    try {
      const token = (import.meta.env.VITE_API_TOKEN as string | undefined) || DEFAULT_API_TOKEN
      const headers: Record<string, string> = { Authorization: `Bearer ${token}` }
      const response = await fetch('/api/status', {
        headers,
        signal: controller.signal,
      })
      backendAvailable.value = response.ok
    } catch {
      // fetch 失败（网络错误/超时/AbortError）→ 后端不可用
      backendAvailable.value = false
    } finally {
      clearTimeout(timeoutId)
      isChecking = false
    }
  }

  /** 手动触发一次检测（立即执行，不等待下一次轮询） */
  const recheck = () => {
    check()
  }

  /** 启动轮询（由 App.vue 在 onMounted 中调用） */
  const start = () => {
    if (intervalId) return
    check()
    intervalId = setInterval(check, CHECK_INTERVAL)
  }

  /** 停止轮询（由 App.vue 在 onUnmounted 中调用） */
  const stop = () => {
    if (intervalId) {
      clearInterval(intervalId)
      intervalId = null
    }
  }

  // HMR 清理：模块热重载时取消旧定时器，避免多个定时器并存
  if (import.meta.hot) {
    import.meta.hot.dispose(() => {
      stop()
    })
  }

  return { backendAvailable, recheck, start, stop }
}

/** 单例实例（闭包内，HMR 重载时自动重置） */
let instance: BackendHealthInstance | null = null

/** 获取或创建单例实例 */
function getInstance(): BackendHealthInstance {
  if (!instance) {
    instance = createBackendHealth()
  }
  return instance
}

/**
 * 后端健康检测组合式函数
 *
 * @returns backendAvailable - 后端是否可用的响应式状态
 * @returns recheck - 手动触发一次检测的方法
 * @returns start - 启动轮询（由 App.vue 在 onMounted 中调用）
 * @returns stop - 停止轮询（由 App.vue 在 onUnmounted 中调用）
 */
export function useBackendHealth(): BackendHealthInstance {
  return getInstance()
}
