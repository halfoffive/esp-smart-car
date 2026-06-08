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

const WS_URL = 'ws://localhost:8080/ws'
const HEARTBEAT_INTERVAL = 30000 // 30秒

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
  odometry: Ref<OdometryData>
  connect: () => void
  disconnect: () => void
  sendCommand: (command: string) => void
  sendSpeed: (speed: number) => void
  sendDriveMode: (mode: number) => void
}

/**
 * 创建 WebSocket 管理实例（工厂函数）
 * 所有状态封装在闭包内，避免模块级全局变量污染
 */
function createWebSocket() {
  // 响应式状态（闭包内部）
  const isConnected = ref(false)
  const videoFrame = ref<string | null>(null)
  const odometry = ref<OdometryData>({
    leftSpeed: 0,
    rightSpeed: 0,
    heading: 0,
    distance: 0,
    timestamp: 0
  })

  // 内部可变状态
  const ws = ref<WebSocket | null>(null)
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null
  let shouldReconnect = true

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
    if (ws.value?.readyState === WebSocket.OPEN) {
      return
    }

    // 重置重连标志，允许自动重连
    shouldReconnect = true

    try {
      const socket = new WebSocket(WS_URL)

      socket.onopen = () => {
        isConnected.value = true
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

        // 自动重连（仅在非主动断开时）
        if (shouldReconnect) {
          reconnectTimer = setTimeout(() => {
            if (shouldReconnect) {
              connect()
            }
          }, 5000)
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
  const sendCommand = (command: string) => {
    if (ws.value?.readyState !== WebSocket.OPEN) {
      return
    }

    const message = {
      type: 'command',
      data: command,
      timestamp: Date.now()
    }

    try {
      ws.value.send(JSON.stringify(message))
    } catch (error) {
      console.error('[WebSocket] 发送命令失败:', error)
    }
  }

  /** 发送速度设置 */
  const sendSpeed = (speed: number) => {
    if (ws.value?.readyState !== WebSocket.OPEN) {
      return
    }

    const message = {
      type: 'speed',
      data: speed.toString(),
      timestamp: Date.now()
    }

    try {
      ws.value.send(JSON.stringify(message))
    } catch (error) {
      console.error('[WebSocket] 发送速度设置失败:', error)
    }
  }

  /** 发送行走模式切换命令 */
  const sendDriveMode = (mode: number) => {
    if (ws.value?.readyState !== WebSocket.OPEN) {
      return
    }

    const message = {
      type: 'drive_mode',
      mode,
      timestamp: Date.now()
    }

    try {
      ws.value.send(JSON.stringify(message))
    } catch (error) {
      console.error('[WebSocket] 发送行走模式失败:', error)
    }
  }

  return {
    isConnected,
    videoFrame,
    odometry,
    connect,
    disconnect,
    sendCommand,
    sendSpeed,
    sendDriveMode
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
    odometry: state.odometry,
    connect: safeConnect,
    disconnect: safeDisconnect,
    sendCommand: state.sendCommand,
    sendSpeed: state.sendSpeed,
    sendDriveMode: state.sendDriveMode
  }
}
