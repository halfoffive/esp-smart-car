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
 */

import { ref, onMounted, onUnmounted } from 'vue'

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

/**
 * 连接 WebSocket
 */
export const connect = () => {
  if (ws.value?.readyState === WebSocket.OPEN) {
    return
  }
  
  try {
    const socket = new WebSocket(WS_URL)
    
    socket.onopen = () => {
      console.log('[WebSocket] 连接成功')
      isConnected.value = true
      
      // 启动心跳
      startHeartbeat()
    }
    
    socket.onmessage = (event) => {
      try {
        const message = JSON.parse(event.data)
        
        switch (message.type) {
          case 'connected':
            console.log('[WebSocket] 已连接:', message.message)
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
                leftSpeed: message.leftSpeed as number,
                rightSpeed: message.rightSpeed as number,
                heading: message.heading as number,
                distance: message.distance as number,
                timestamp: message.timestamp as number
              }
            }
            break
            
          case 'status':
            console.log('[WebSocket] 状态:', message)
            break
            
          default:
            console.log('[WebSocket] 收到消息:', message)
        }
      } catch (e) {
        console.error('[WebSocket] 解析消息失败:', e)
      }
    }
    
    socket.onerror = (error) => {
      console.error('[WebSocket] 错误:', error)
      isConnected.value = false
    }
    
    socket.onclose = () => {
      console.log('[WebSocket] 连接关闭')
      isConnected.value = false
      stopHeartbeat()
      
      // 自动重连
      reconnectTimer = setTimeout(() => {
        console.log('[WebSocket] 尝试重连...')
        connect()
      }, 5000) as unknown as number
    }
    
    ws.value = socket
  } catch (e) {
    console.error('[WebSocket] 连接失败:', e)
    isConnected.value = false
  }
}

/**
 * 断开 WebSocket
 */
export const disconnect = () => {
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
    console.warn('[WebSocket] 未连接，无法发送命令')
    return
  }
  
  const message = {
    type: 'command',
    data: command,
    timestamp: Date.now()
  }
  
  ws.value.send(JSON.stringify(message))
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
  
  ws.value.send(JSON.stringify(message))
}

/**
 * 启动心跳
 */
const startHeartbeat = () => {
  stopHeartbeat()
  
  heartbeatTimer = setInterval(() => {
    if (ws.value?.readyState === WebSocket.OPEN) {
      ws.value.send(JSON.stringify({
        type: 'heartbeat',
        timestamp: Date.now()
      }))
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
  
  ws.value.send(JSON.stringify(message))
}

/**
 * 组合式函数
 */
export const useWebSocket = () => {
  onMounted(() => {
    if (!isConnected.value) {
      connect()
    }
  })
  
  onUnmounted(() => {
    disconnect()
  })
  
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
