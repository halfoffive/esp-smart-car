/**
 * 状态轮询组合式函数
 * 统一管理 /api/status 轮询，避免多组件重复请求同一端点
 *
 * 功能：
 * 1. 单一 1 秒轮询间隔，所有组件共享数据
 * 2. 引用计数自动启停轮询
 * 3. 提供响应式状态供组件消费
 *
 * 设计模式：闭包 + 单例 + 引用计数
 * - 首个组件挂载时启动轮询，最后一个组件卸载时停止
 * - 避免多个组件各自轮询造成请求浪费
 */

import { ref, onMounted, onUnmounted } from 'vue'
import type { Ref } from 'vue'
import { useApi } from './useApi'

/** 状态数据接口（与后端 StatusResponse 对齐） */
export interface StatusData {
  serial_status: string
  port_name: string | null
  baud_rate: number | null
  frame_count: number
  bytes_sent: number
  current_speed: number
  ws_clients: number
  uptime: number
  version: string
  left_speed: number
  right_speed: number
  heading: number
  total_distance: number
  command_count: number
}

const POLL_INTERVAL = 1000 // 1 秒轮询间隔

/** 创建状态轮询实例（工厂函数） */
function createStatusPoller() {
  const { get } = useApi()

  // 响应式状态
  const status: Ref<StatusData> = ref({
    serial_status: '未连接',
    port_name: null,
    baud_rate: null,
    frame_count: 0,
    bytes_sent: 0,
    current_speed: 5,
    ws_clients: 0,
    uptime: 0,
    version: '',
    left_speed: 0,
    right_speed: 0,
    heading: 0,
    total_distance: 0,
    command_count: 0,
  })
  const isPolling = ref(false)

  // 内部可变状态
  let interval: ReturnType<typeof setInterval> | null = null
  let refCount = 0 // 引用计数

  /** 拉取一次状态 */
  const fetchStatus = async () => {
    try {
      const data = await get<StatusData>('/api/status')
      status.value = data
    } catch (error) {
      // 仅在开发环境输出错误日志，避免生产环境控制台污染
      if (import.meta.env.DEV) {
        console.error('[useStatus] 状态查询失败:', error)
      }
    }
  }

  /** 启动轮询 */
  const startPolling = () => {
    if (interval !== null) return
    isPolling.value = true
    fetchStatus()
    interval = setInterval(fetchStatus, POLL_INTERVAL)
  }

  /** 停止轮询 */
  const stopPolling = () => {
    if (interval !== null) {
      clearInterval(interval)
      interval = null
    }
    isPolling.value = false
  }

  /** 增加引用计数 */
  const addRef = () => {
    refCount++
    if (refCount === 1) {
      startPolling()
    }
  }

  /** 减少引用计数 */
  const releaseRef = () => {
    refCount = Math.max(0, refCount - 1)
    if (refCount === 0) {
      stopPolling()
    }
  }

  return { status, isPolling, addRef, releaseRef }
}

/** 单例实例 */
let instance: ReturnType<typeof createStatusPoller> | null = null

/** 获取或创建单例实例 */
function getInstance(): ReturnType<typeof createStatusPoller> {
  if (!instance) {
    instance = createStatusPoller()
  }
  return instance
}

/**
 * 状态轮询组合式函数
 *
 * 自动管理轮询生命周期：组件挂载时增加引用计数并启动轮询，
 * 卸载时减少引用计数，最后一个组件卸载后停止轮询。
 *
 * @returns status - 响应式状态数据
 * @returns isPolling - 是否正在轮询
 */
export function useStatus() {
  const poller = getInstance()

  onMounted(() => {
    poller.addRef()
  })

  onUnmounted(() => {
    poller.releaseRef()
  })

  return {
    status: poller.status,
    isPolling: poller.isPolling,
  }
}
