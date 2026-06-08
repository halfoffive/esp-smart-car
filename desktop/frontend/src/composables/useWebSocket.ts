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
 * 设计模式：单管理员模式
 * - 只有 owner=true 的调用者才能执行 connect() 和 disconnect()
 * - 其他调用者只消费状态（isConnected, videoFrame, odometry 等）
 * - 防止多组件卸载时意外断开全局 WebSocket
 */

import { ref } from 'vue'

const WS_URL = 'ws://localhost:8080/ws'
const HEARTBEAT_INTERVAL = 30000 // 30秒

// 测速数据接口
export interface OdometryData {
  leftSpeed: number    // 左轮速度(mm/s)
  rightSpeed: number   // 右轮速度(mm/s)
  heading: number      // 航向角(弧度)
  distance: number     // 总行走距离(mm)
  timestamp: number    // 时间戳
}

// 全局状态
const isConnected = ref(false)
const videoFrame = ref<string | null>(null)
const odometry = ref<OdometryData>({
  leftSpeed: 0,
  rightSpeed: 0,
  heading: 0,
  distance: 0,
  timestamp: 0
})
const ws = ref<WebSocket | null>(null)
let heartbeatTimer: number | null = null
let reconnectTimer: number | null = null
let shouldReconnect = true

/**
 * 连接 WebSocket
 */
export const connect = () => {
  if (ws.value?.readyState === WebSocket.OPEN) {
    return
  }

  // 重置重连标志，允许自动重连
  shouldReconnect = true

  try {
    const socket = new WebSocket(WS_URL)

    socket.onopen = () => {
      isConnected.value = true

      // 启动心跳
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
            // 接收测速数据
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
      } catch {
        // JSON 解析失败，忽略非标准消息
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
        }, 5000) as unknown as number
      }
    }

    ws.value = socket
  } catch {
    isConnected.value = false
  }
}

/**
 * 断开 WebSocket
 */
export const disconnect = () => {
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

/**
 * 发送命令
 */
export const sendCommand = (command: string) => {
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
  } catch {
    // send 失败时连接已断开，onclose 会自动处理重连
  }
}

/**
 * 发送速度设置
 */
export const sendSpeed = (speed: number) => {
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
  } catch {
    // send 失败时连接已断开，onclose 会自动处理重连
  }
}

/**
 * 启动心跳
 */
const startHeartbeat = () => {
  stopHeartbeat()

  heartbeatTimer = setInterval(() => {
    if (ws.value?.readyState === WebSocket.OPEN) {
      try {
        ws.value.send(JSON.stringify({
          type: 'heartbeat',
          timestamp: Date.now()
        }))
      } catch {
        // send 失败时连接已断开，onclose 会自动处理重连
      }
    }
  }, HEARTBEAT_INTERVAL) as unknown as number
}

/**
 * 停止心跳
 */
const stopHeartbeat = () => {
  if (heartbeatTimer) {
    clearInterval(heartbeatTimer)
    heartbeatTimer = null
  }
}

/**
 * 发送行走模式切换命令
 */
export const sendDriveMode = (mode: number) => {
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
  } catch {
    // send 失败时连接已断开，onclose 会自动处理重连
  }
}

/**
 * 组合式函数
 * 
 * @param owner - 是否为管理员组件。只有管理员才能调用 connect() 和 disconnect()
 *              其他组件只应消费状态（isConnected, videoFrame, odometry 等）
 */
export const useWebSocket = (owner = false) => {
  // 单管理员模式：只有 owner 才能执行连接管理操作
  const safeConnect = owner
    ? connect
    : () => {
        // 非管理员组件无法调用 connect()
      }

  const safeDisconnect = owner
    ? disconnect
    : () => {
        // 非管理员组件无法调用 disconnect()
      }

  return {
    isConnected,
    videoFrame,
    odometry,
    connect: safeConnect,
    disconnect: safeDisconnect,
    sendCommand,
    sendSpeed,
    sendDriveMode
  }
}
