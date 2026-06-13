/**
 * WebSocket 组合式函数
 * 管理 WebSocket 连接，处理视频帧和命令传输
 * 
 * 功能：
 * 1. 连接/断开 WebSocket
 * 2. 发送控制命令
 * 3. 接收视频帧
 * 4. 接收测速数据
 * 5. 心跳保活
 * 
 * 设计模式：闭包 + 单例模式
 * - 所有状态封装在工厂函数闭包中，避免模块级全局变量（HMR 友好）
 * - 只有 owner=true 的调用者才能执行 connect() 和 disconnect()
 * - 其他调用者只消费状态（isConnected, videoFrame, odometry 等）
 * - 防止多组件卸载时意外断开全局 WebSocket
 */

import { ref } from 'vue'
import type { Ref } from 'vue'

/** 根据当前页面协议动态构建 WebSocket URL（开发/生产通用） */
const WS_URL = `${window.location.protocol === 'https:' ? 'wss:' : 'ws:'}//${window.location.host}/ws`
const HEARTBEAT_INTERVAL = 30000 // 30秒
const MAX_RETRY_COUNT = 10       // 最大重连次数
const INITIAL_RETRY_DELAY = 1000 // 初始重连延迟（毫秒）
const MAX_RETRY_DELAY = 30000    // 最大重连延迟（毫秒）

/** 测速数据接口 */
export interface OdometryData {
  leftSpeed: number    // 左轮速度(mm/s)
  rightSpeed: number   // 右轮速度(mm/s)
  heading: number      // 航向角(弧度)
  distance: number     // 总行走距离(mm)
  timestamp: number    // 时间戳
}

/** WebSocket 实例接口 */
interface WebSocketInstance {
  isConnected: Ref<boolean>
  videoFrame: Ref<string | null>
  videoFps: Ref<number>
  odometry: Ref<OdometryData>
  availablePorts: Ref<string[]>
  connect: () => void
  disconnect: () => void
  sendCommand: (command: string) => boolean
  sendSpeed: (speed: number) => boolean
  sendDriveMode: (mode: number) => boolean
  sendMacConfig: (mac: string) => boolean
}

/**
 * 创建 WebSocket 管理实例（工厂函数）
 * 所有状态封装在闭包内，避免模块级全局变量污染
 */
function createWebSocket() {
  // 响应式状态（闭包内部）
  const isConnected = ref(false)
  const videoFrame = ref<string | null>(null)
  const videoFps = ref(0)
  const odometry = ref<OdometryData>({
    leftSpeed: 0,
    rightSpeed: 0,
    heading: 0,
    distance: 0,
    timestamp: 0
  })
  const availablePorts = ref<string[]>([])

  // 内部可变状态
  const ws = ref<WebSocket | null>(null)
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null
  let shouldReconnect = true
  let retryCount = 0

  /** 启动心跳 */
  const startHeartbeat = () => {
    stopHeartbeat()

    heartbeatTimer = setInterval(() => {
      if (ws.value?.readyState === WebSocket.OPEN) {
        try {
          ws.value.send(JSON.stringify({
            type: 'heartbeat',
            timestamp: Date.now()
          }))
        } catch (error) {
          console.error('[WebSocket] 心跳发送失败:', error)
        }
      }
    }, HEARTBEAT_INTERVAL)
  }

  /** 停止心跳 */
  const stopHeartbeat = () => {
    if (heartbeatTimer) {
      clearInterval(heartbeatTimer)
      heartbeatTimer = null
    }
  }

  /** 连接 WebSocket */
  const connect = () => {
    // 关闭已有连接前先清理心跳定时器，防止定时器累积
    stopHeartbeat()

    // 关闭已有连接（包括 CONNECTING/OPEN/CLOSING 状态），防止旧连接的回调干扰
    if (ws.value && ws.value.readyState !== WebSocket.CLOSED) {
      shouldReconnect = false
      ws.value.onopen = null
      ws.value.onclose = null
      ws.value.onerror = null
      ws.value.onmessage = null
      ws.value.close()
      // 等待一小段时间确保旧连接的 onclose 不会触发重连
      ws.value = null
    }

    // 重置重连标志，允许自动重连
    shouldReconnect = true

    try {
      const socket = new WebSocket(WS_URL)

      socket.onopen = () => {
        isConnected.value = true
        // 连接成功，重置重连计数
        retryCount = 0
        // 清理重连定时器，防止手动重连后定时器仍触发创建多余连接
        if (reconnectTimer) {
          clearTimeout(reconnectTimer)
          reconnectTimer = null
        }
        startHeartbeat()
      }

      socket.onmessage = (event) => {
        try {
          const message = JSON.parse(event.data)

          switch (message.type) {
            case 'connected':
              break

            case 'video':
              // 接收视频帧
              if (message.data) {
                videoFrame.value = `data:image/jpeg;base64,${message.data}`
              }
              break

            case 'odometry':
              // 接收测速数据（运行时类型校验）
              if (message.leftSpeed !== undefined) {
                odometry.value = {
                  leftSpeed: typeof message.leftSpeed === 'number' ? message.leftSpeed : 0,
                  rightSpeed: typeof message.rightSpeed === 'number' ? message.rightSpeed : 0,
                  heading: typeof message.heading === 'number' ? message.heading : 0,
                  distance: typeof message.distance === 'number' ? message.distance : 0,
                  timestamp: typeof message.timestamp === 'number' ? message.timestamp : 0
                }
              }
              break

            case 'status':
              break

            case 'port_list':
              // 接收串口列表推送
              if (Array.isArray(message.ports)) {
                availablePorts.value = message.ports as string[]
              }
              break

            default:
              break
          }
        } catch (error) {
          // JSON 解析失败，忽略非标准消息
          console.error('[WebSocket] 消息解析失败:', error)
        }
      }

      socket.onerror = () => {
        isConnected.value = false
      }

      socket.onclose = () => {
        isConnected.value = false
        stopHeartbeat()

        // 自动重连（仅在非主动断开且未超过最大重试次数时）
        if (shouldReconnect && retryCount < MAX_RETRY_COUNT) {
          // 指数退避：1s, 2s, 4s, 8s, 16s, 30s, 30s, ...
          const delay = Math.min(INITIAL_RETRY_DELAY * Math.pow(2, retryCount), MAX_RETRY_DELAY)
          retryCount++
          reconnectTimer = setTimeout(() => {
            if (shouldReconnect) {
              connect()
            }
          }, delay)
        }
      }

      ws.value = socket
    } catch (error) {
      console.error('[WebSocket] 连接创建失败:', error)
      isConnected.value = false
    }
  }

  /** 断开 WebSocket */
  const disconnect = () => {
    // 先设置标志，防止 onclose handler 触发自动重连
    shouldReconnect = false

    stopHeartbeat()

    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
    }

    if (ws.value) {
      ws.value.close()
      ws.value = null
    }

    isConnected.value = false
    videoFrame.value = null
  }

  /** 发送命令 */
  const sendCommand = (command: string): boolean => {
    if (ws.value?.readyState !== WebSocket.OPEN) {
      return false
    }

    const message = {
      type: 'command',
      data: command,
      timestamp: Date.now()
    }

    try {
      ws.value.send(JSON.stringify(message))
      return true
    } catch (error) {
      console.error('[WebSocket] 发送命令失败:', error)
      return false
    }
  }

  /** 发送速度设置 */
  const sendSpeed = (speed: number): boolean => {
    if (ws.value?.readyState !== WebSocket.OPEN) {
      return false
    }

    const message = {
      type: 'speed',
      data: speed.toString(),
      timestamp: Date.now()
    }

    try {
      ws.value.send(JSON.stringify(message))
      return true
    } catch (error) {
      console.error('[WebSocket] 发送速度设置失败:', error)
      return false
    }
  }

  /** 发送行走模式切换命令 */
  const sendDriveMode = (mode: number): boolean => {
    if (ws.value?.readyState !== WebSocket.OPEN) {
      return false
    }

    const message = {
      type: 'drive_mode',
      mode,
      timestamp: Date.now()
    }

    try {
      ws.value.send(JSON.stringify(message))
      return true
    } catch (error) {
      console.error('[WebSocket] 发送行走模式失败:', error)
      return false
    }
  }

  /** 发送MAC地址配置命令 */
  const sendMacConfig = (mac: string): boolean => {
    if (ws.value?.readyState !== WebSocket.OPEN) {
      return false
    }

    const message = {
      type: 'mac_config',
      mac,
      timestamp: Date.now()
    }

    try {
      ws.value.send(JSON.stringify(message))
      return true
    } catch (error) {
      console.error('[WebSocket] 发送MAC配置失败:', error)
      return false
    }
  }

  return {
    isConnected,
    videoFrame,
    videoFps,
    odometry,
    availablePorts,
    connect,
    disconnect,
    sendCommand,
    sendSpeed,
    sendDriveMode,
    sendMacConfig
  }
}

/** 单例实例（闭包内，HMR 重载时自动重置） */
let instance: ReturnType<typeof createWebSocket> | null = null

/** 获取或创建单例实例 */
function getInstance(): ReturnType<typeof createWebSocket> {
  if (!instance) {
    instance = createWebSocket()
  }
  return instance
}

/**
 * WebSocket 组合式函数
 * 
 * @param owner - 是否为管理员组件。只有管理员才能调用 connect() 和 disconnect()
 *              其他组件只应消费状态（isConnected, videoFrame, odometry 等）
 */
export const useWebSocket = (owner = false): WebSocketInstance => {
  const state = getInstance()

  // 单管理员模式：只有 owner 才能执行连接管理操作
  const safeConnect = owner
    ? state.connect
    : () => {
        // 非管理员组件无法调用 connect()
      }

  const safeDisconnect = owner
    ? state.disconnect
    : () => {
        // 非管理员组件无法调用 disconnect()
      }

  return {
    isConnected: state.isConnected,
    videoFrame: state.videoFrame,
    videoFps: state.videoFps,
    odometry: state.odometry,
    availablePorts: state.availablePorts,
    connect: safeConnect,
    disconnect: safeDisconnect,
    sendCommand: state.sendCommand,
    sendSpeed: state.sendSpeed,
    sendDriveMode: state.sendDriveMode,
    sendMacConfig: state.sendMacConfig
  }
}
