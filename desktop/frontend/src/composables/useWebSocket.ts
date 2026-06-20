/**
 * WebSocket 组合式函数
 * 管理 WebSocket 连接，处理视频帧和命令传输
 *
 * 功能：
 * 1. 连接/断开 WebSocket
 * 2. 发送控制命令
 * 3. 接收视频帧
 * 4. 接收测速数据
 * 5. 心跳保活（含响应超时检测）
 * 6. 接收链路状态（dongle/车载配对/在线状态）
 * 7. 接收系统状态（替代 /api/status 轮询）
 *
 * 设计模式：闭包 + 单例模式
 * - 所有状态封装在工厂函数闭包中，避免模块级全局变量（HMR 友好）
 * - 只有 owner=true 的调用者才能执行 connect() 和 disconnect()
 * - 其他调用者只消费状态（isConnected, videoFrame, odometry 等）
 * - 防止多组件卸载时意外断开全局 WebSocket
 */

import { ref } from 'vue'
import type { Ref } from 'vue'

/**
 * 构建 WebSocket URL
 * 支持反向代理子路径部署（如 https://example.com/smartcar/）
 * - pathname="/" → ws://host/ws
 * - pathname="/smartcar/" → ws://host/smartcar/ws
 * - pathname="/smartcar" → ws://host/smartcar/ws
 */
const WS_PROTOCOL = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
const WS_HOST = window.location.host
const WS_PATH = window.location.pathname.replace(/\/$/, '')
const WS_BASE_URL = `${WS_PROTOCOL}//${WS_HOST}${WS_PATH}/ws`

/** 根据 API Token 构建带认证参数的 WebSocket URL */
const buildWsUrl = (token: string): string => `${WS_BASE_URL}?token=${encodeURIComponent(token)}`
const HEARTBEAT_INTERVAL = 30000   // 心跳发送间隔（毫秒）
const HEARTBEAT_TIMEOUT = 90000    // 心跳响应超时（毫秒），后端同样按 90 秒判定死连接
const MAX_RETRY_COUNT = 10         // 最大重连次数
const INITIAL_RETRY_DELAY = 1000   // 初始重连延迟（毫秒）
const MAX_RETRY_DELAY = 30000      // 最大重连延迟（毫秒）
const CONNECT_TIMEOUT = 5000       // 连接超时（毫秒）

/** 测速数据接口 */
export interface OdometryData {
  leftSpeed: number    // 左轮速度(mm/s)
  rightSpeed: number   // 右轮速度(mm/s)
  heading: number      // 航向角(弧度)
  distance: number     // 总行走距离(mm)
  timestamp: number    // 时间戳
}

/** BLE 设备接口 */
export interface BleDevice {
  name: string
  /** BLE 广播 MAC 地址 */
  mac: string
  /** WiFi (ESP-NOW) MAC 地址，从 Manufacturer Data 提取。仅车载 C6 等设备会广播此字段 */
  wifiMac?: string
  rssi: number
}

/**
 * 系统状态接口（与后端 StatusResponse 对齐，camelCase）
 * 由后端通过 WS status 消息推送，替代前端 /api/status 轮询
 */
export interface StatusData {
  serialStatus: string    // 串口连接状态（"未连接"/"连接中"/"已连接"/"错误: ..."）
  frameCount: number      // 已接收帧数
  currentSpeed: number    // 当前速度 PWM 占空比（0-255）
  wsClients: number       // WebSocket 连接数
  uptime: number          // 运行时长（秒）
  commandCount: number    // 已发送命令数
  driveMode?: number      // 后端推送的行走模式（0=普通, 1=直线修正, 2=航向锁定）
}

/**
 * 链路状态接口（与后端 LinkStatus 对齐，camelCase）
 * 由后端通过 WS link_status 消息推送
 * - dongleOk: Dongle 是否正常响应探测
 * - carPaired: 车载 ESP-NOW 是否已与 Dongle 配对
 * - lastOdomMs: 距上次收到车载数据的毫秒数（>10000 视为离线）
 */
export interface LinkStatus {
  dongleOk: boolean
  carPaired: boolean
  lastOdomMs: number
}

/** WebSocket 实例接口 */
interface WebSocketInstance {
  isConnected: Ref<boolean>
  isConnecting: Ref<boolean>
  connectionError: Ref<string | null>
  videoFrame: Ref<string | null>
  videoFps: Ref<number>
  odometry: Ref<OdometryData>
  availablePorts: Ref<string[]>
  bleDevices: Ref<BleDevice[]>
  status: Ref<StatusData>
  linkStatus: Ref<LinkStatus>
  connect: () => Promise<void>
  disconnect: () => void
  sendCommand: (command: string) => boolean
  sendSpeed: (speed: number) => boolean
  sendDriveMode: (mode: number) => boolean
  sendMacConfig: (mac: string) => boolean
  sendBleScan: () => boolean
}

/**
 * 创建 WebSocket 管理实例（工厂函数）
 * 所有状态封装在闭包内，避免模块级全局变量污染
 */
function createWebSocket() {
  // 响应式状态（闭包内部）
  const isConnected = ref(false)
  const isConnecting = ref(false)
  const connectionError = ref<string | null>(null)
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
  const bleDevices = ref<BleDevice[]>([])
  // 系统状态（由后端 WS status 消息推送，替代 /api/status 轮询）
  const status = ref<StatusData>({
    serialStatus: '未连接',
    frameCount: 0,
    currentSpeed: 0,
    wsClients: 0,
    uptime: 0,
    commandCount: 0,
  })
  // 链路状态（由后端 WS link_status 消息推送）
  const linkStatus = ref<LinkStatus>({
    dongleOk: false,
    carPaired: false,
    lastOdomMs: 0,
  })

  // 内部可变状态
  const ws = ref<WebSocket | null>(null)
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let heartbeatResponseTimer: ReturnType<typeof setTimeout> | null = null
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null
  let connectTimeoutTimer: ReturnType<typeof setTimeout> | null = null
  let shouldReconnect = true
  let retryCount = 0
  // 连接世代计数器：防止 connect() 与 disconnect() 竞态
  let connectGeneration = 0
  // FPS 计算：基于视频帧到达频率（供 StatusBar 等组件消费）
  let frameCount = 0
  let lastFpsUpdate = Date.now()

  /** 彻底清理所有定时器 */
  const clearAllTimers = () => {
    stopHeartbeat()
    if (heartbeatResponseTimer) {
      clearTimeout(heartbeatResponseTimer)
      heartbeatResponseTimer = null
    }
    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
    }
    if (connectTimeoutTimer) {
      clearTimeout(connectTimeoutTimer)
      connectTimeoutTimer = null
    }
  }

  /** 重置连接相关的响应式状态 */
  const resetConnectionState = () => {
    isConnected.value = false
    isConnecting.value = false
    connectionError.value = null
  }

  /** 重置所有业务状态（彻底断开时调用） */
  const resetAllState = () => {
    resetConnectionState()
    videoFrame.value = null
    videoFps.value = 0
    frameCount = 0
    lastFpsUpdate = Date.now()
    odometry.value = {
      leftSpeed: 0,
      rightSpeed: 0,
      heading: 0,
      distance: 0,
      timestamp: 0
    }
    availablePorts.value = []
    bleDevices.value = []
    status.value = {
      serialStatus: '未连接',
      frameCount: 0,
      currentSpeed: 0,
      wsClients: 0,
      uptime: 0,
      commandCount: 0,
    }
    linkStatus.value = {
      dongleOk: false,
      carPaired: false,
      lastOdomMs: 0,
    }
  }

  /** 启动心跳响应超时检测 */
  const startHeartbeatResponseTimer = () => {
    if (heartbeatResponseTimer) {
      clearTimeout(heartbeatResponseTimer)
    }
    heartbeatResponseTimer = setTimeout(() => {
      console.warn('[WebSocket] 心跳响应超时，连接可能已失效')
      connectionError.value = '心跳超时，连接可能已断开'
      // 主动关闭以触发重连或错误提示
      ws.value?.close()
    }, HEARTBEAT_TIMEOUT)
  }

  /** 停止心跳响应超时检测 */
  const stopHeartbeatResponseTimer = () => {
    if (heartbeatResponseTimer) {
      clearTimeout(heartbeatResponseTimer)
      heartbeatResponseTimer = null
    }
  }

  /** 启动心跳 */
  const startHeartbeat = () => {
    stopHeartbeat()
    startHeartbeatResponseTimer()

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
    stopHeartbeatResponseTimer()
  }

  /** 连接 WebSocket（外部入口：手动连接时重置重连计数） */
  const connect = async (): Promise<void> => {
    return internalConnect(false)
  }

  /** 内部连接实现（支持手动连接与自动重连） */
  const internalConnect = async (isRetry: boolean): Promise<void> => {
    // 重入保护：若正在连接中，直接返回
    if (isConnecting.value) {
      return Promise.reject(new Error('连接正在进行中，请勿重复调用'))
    }

    // 认证校验：未配置 token 时给出明确错误
    const token = import.meta.env.VITE_API_TOKEN
    if (!token) {
      isConnecting.value = false
      const error = 'VITE_API_TOKEN 未配置，请在 .env 文件中设置后刷新页面'
      connectionError.value = error
      return Promise.reject(new Error(error))
    }

    isConnecting.value = true
    connectionError.value = null
    clearAllTimers()

    // 新连接世代，disconnect() 可通过递增此值取消本次连接
    const myGeneration = ++connectGeneration

    // 关闭已有连接（包括 CONNECTING/OPEN/CLOSING 状态），防止旧连接的回调干扰
    if (ws.value && ws.value.readyState !== WebSocket.CLOSED) {
      shouldReconnect = false
      const oldSocket = ws.value
      oldSocket.onopen = null
      oldSocket.onclose = null
      oldSocket.onerror = null
      oldSocket.onmessage = null
      oldSocket.close()
      ws.value = null
      // 等待一小段时间确保旧连接的 onclose 回调完成后再创建新连接
      await new Promise(r => setTimeout(r, 50))
      // 等待期间若 disconnect() 被调用，世代会变化，本次连接应中止
      if (myGeneration !== connectGeneration) {
        isConnecting.value = false
        return Promise.reject(new Error('连接已被取消'))
      }
    }

    // 重置重连标志，允许自动重连
    shouldReconnect = true

    // 手动连接时重置重连计数，自动重连时保留并递增
    if (!isRetry) {
      retryCount = 0
    }

    // 再次校验世代，防止等待期间被取消
    if (myGeneration !== connectGeneration) {
      isConnecting.value = false
      return Promise.reject(new Error('连接已被取消'))
    }

    return new Promise<void>((resolve, reject) => {
      let resolved = false
      let rejected = false

      const finalizeResolve = () => {
        if (resolved || rejected) return
        resolved = true
        isConnecting.value = false
        connectionError.value = null
        resolve()
      }

      const finalizeReject = (reason: string) => {
        if (resolved || rejected) return
        rejected = true
        isConnecting.value = false
        connectionError.value = reason
        // 清理 socket 引用和事件处理器，避免残留回调
        if (ws.value) {
          ws.value.onopen = null
          ws.value.onclose = null
          ws.value.onerror = null
          ws.value.onmessage = null
          ws.value.close()
          ws.value = null
        }
        reject(new Error(reason))
      }

      try {
        const socket = new WebSocket(buildWsUrl(token))

        // 连接超时处理
        connectTimeoutTimer = setTimeout(() => {
          finalizeReject('WebSocket 连接超时')
        }, CONNECT_TIMEOUT)

        socket.onopen = () => {
          if (connectTimeoutTimer) {
            clearTimeout(connectTimeoutTimer)
            connectTimeoutTimer = null
          }
          isConnected.value = true
          // 连接成功，重置重连计数
          retryCount = 0
          // 清理重连定时器，防止手动重连后定时器仍触发创建多余连接
          if (reconnectTimer) {
            clearTimeout(reconnectTimer)
            reconnectTimer = null
          }
          startHeartbeat()
          finalizeResolve()
        }

        socket.onmessage = (event) => {
          try {
            const message: unknown = JSON.parse(event.data)

            // 基础类型守卫：非对象消息无法识别类型，直接忽略
            if (typeof message !== 'object' || message === null) {
              return
            }
            const msg = message as Record<string, unknown>

            // 收到任何服务端消息都视为心跳响应，重置超时检测
            if (heartbeatResponseTimer) {
              startHeartbeatResponseTimer()
            }

            switch (msg.type) {
              case 'connected':
                break

              case 'video':
                // 接收视频帧
                if (msg.data) {
                  videoFrame.value = `data:image/jpeg;base64,${msg.data}`
                  // 更新 videoFps：每秒统计接收到的视频帧数
                  frameCount++
                  const now = Date.now()
                  if (now - lastFpsUpdate >= 1000) {
                    videoFps.value = frameCount
                    frameCount = 0
                    lastFpsUpdate = now
                  }
                }
                break

              case 'odometry':
                // 接收测速数据（运行时类型校验）
                if (msg.leftSpeed !== undefined) {
                  odometry.value = {
                    leftSpeed: typeof msg.leftSpeed === 'number' ? msg.leftSpeed : 0,
                    rightSpeed: typeof msg.rightSpeed === 'number' ? msg.rightSpeed : 0,
                    heading: typeof msg.heading === 'number' ? msg.heading : 0,
                    distance: typeof msg.distance === 'number' ? msg.distance : 0,
                    timestamp: typeof msg.timestamp === 'number' ? msg.timestamp : 0
                  }
                }
                break

              case 'status':
                // 接收系统状态（后端 snake_case → 前端 camelCase）
                // 后端推送字段：serial_status/frame_count/current_speed/ws_clients/uptime/command_count/drive_mode
                if (typeof msg.serial_status === 'string') {
                  status.value = {
                    serialStatus: msg.serial_status,
                    frameCount: typeof msg.frame_count === 'number' ? msg.frame_count : 0,
                    currentSpeed: typeof msg.current_speed === 'number' ? msg.current_speed : 0,
                    wsClients: typeof msg.ws_clients === 'number' ? msg.ws_clients : 0,
                    uptime: typeof msg.uptime === 'number' ? msg.uptime : 0,
                    commandCount: typeof msg.command_count === 'number' ? msg.command_count : 0,
                    driveMode: typeof msg.drive_mode === 'number' ? msg.drive_mode : undefined,
                  }
                }
                break

              case 'link_status':
                // 接收链路状态（后端 snake_case → 前端 camelCase）
                // 后端推送字段：dongle_ok/car_paired/last_odom_ms
                if (typeof msg.dongle_ok === 'boolean') {
                  linkStatus.value = {
                    dongleOk: msg.dongle_ok,
                    carPaired: typeof msg.car_paired === 'boolean' ? msg.car_paired : false,
                    lastOdomMs: typeof msg.last_odom_ms === 'number' ? msg.last_odom_ms : 0,
                  }
                }
                break

              case 'port_list':
                // 接收串口列表推送
                if (Array.isArray(msg.ports)) {
                  availablePorts.value = (msg.ports as unknown[]).filter((p): p is string => typeof p === 'string')
                }
                break

              case 'ble_devices':
                // 接收 BLE 设备列表（wifi_mac 从后端 JSON 映射到 wifiMac）
                if (Array.isArray(msg.devices)) {
                  bleDevices.value = msg.devices.filter((d: unknown): d is BleDevice => {
                    if (typeof d !== 'object' || d === null) return false
                    const dev = d as Record<string, unknown>
                    return typeof dev.name === 'string' && typeof dev.mac === 'string' && typeof dev.rssi === 'number'
                  }).map((d: BleDevice & { wifi_mac?: unknown }) => ({
                    name: d.name,
                    mac: d.mac,
                    rssi: d.rssi,
                    wifiMac: typeof d.wifi_mac === 'string' ? d.wifi_mac : undefined,
                  }))
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
          if (connectTimeoutTimer) {
            clearTimeout(connectTimeoutTimer)
            connectTimeoutTimer = null
          }
          isConnected.value = false
          // onerror 后不直接 reject，等待 onclose 统一处理连接失败
        }

        socket.onclose = (event) => {
          if (connectTimeoutTimer) {
            clearTimeout(connectTimeoutTimer)
            connectTimeoutTimer = null
          }
          isConnected.value = false
          stopHeartbeat()

          // 首次连接失败时（尚未 resolve），reject Promise
          if (!resolved && !rejected) {
            finalizeReject(event.wasClean ? 'WebSocket 连接已关闭' : 'WebSocket 连接失败')
            return
          }

          // 自动重连（仅在非主动断开且未超过最大重试次数时）
          if (shouldReconnect && retryCount < MAX_RETRY_COUNT) {
            // 指数退避：1s, 2s, 4s, 8s, 16s, 30s, 30s, ...
            const delay = Math.min(INITIAL_RETRY_DELAY * Math.pow(2, retryCount), MAX_RETRY_DELAY)
            retryCount++
            connectionError.value = `连接断开，${delay / 1000} 秒后重连（第 ${retryCount}/${MAX_RETRY_COUNT} 次）`
            reconnectTimer = setTimeout(() => {
              if (shouldReconnect) {
                internalConnect(true).catch(() => {
                  // 重连失败由 onclose 继续处理，无需额外操作
                })
              }
            }, delay)
          } else if (shouldReconnect) {
            // 超过最大重连次数，停止重连并提示用户
            connectionError.value = '连接已断开，自动重连次数已耗尽，请手动刷新页面或点击连接'
            console.error('[WebSocket] 自动重连次数已耗尽')
          }
        }

        ws.value = socket
      } catch (error) {
        if (connectTimeoutTimer) {
          clearTimeout(connectTimeoutTimer)
          connectTimeoutTimer = null
        }
        finalizeReject(`WebSocket 连接创建失败: ${error instanceof Error ? error.message : String(error)}`)
      }
    })
  }

  /** 断开 WebSocket */
  const disconnect = () => {
    // 先设置标志，防止 onclose handler 触发自动重连
    shouldReconnect = false
    // 递增连接世代，取消任何正在等待的 connect()
    connectGeneration++

    clearAllTimers()

    if (ws.value) {
      // 清空事件处理器，避免关闭后残留回调修改状态
      ws.value.onopen = null
      ws.value.onclose = null
      ws.value.onerror = null
      ws.value.onmessage = null
      ws.value.close()
      ws.value = null
    }

    resetAllState()
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

  /** 发送速度设置（speed 为 0-255 PWM 占空比） */
  const sendSpeed = (speed: number): boolean => {
    if (speed < 0 || speed > 255) {
      console.error('[WebSocket] 速度值越界:', speed)
      return false
    }
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

  /** 发送 BLE 扫描命令 */
  const sendBleScan = (): boolean => {
    if (ws.value?.readyState !== WebSocket.OPEN) {
      return false
    }

    const message = {
      type: 'ble_scan',
      timestamp: Date.now()
    }

    try {
      ws.value.send(JSON.stringify(message))
      return true
    } catch (error) {
      console.error('[WebSocket] 发送 BLE 扫描命令失败:', error)
      return false
    }
  }

  return {
    isConnected,
    isConnecting,
    connectionError,
    videoFrame,
    videoFps,
    odometry,
    availablePorts,
    bleDevices,
    status,
    linkStatus,
    connect,
    disconnect,
    sendCommand,
    sendSpeed,
    sendDriveMode,
    sendMacConfig,
    sendBleScan
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
    : async () => {
        // 非管理员组件无法调用 connect()
        if (import.meta.env.DEV) {
          console.warn('[useWebSocket] 非管理员组件尝试调用 connect()，已忽略。请使用 useWebSocket(true) 作为管理员。')
        }
        return Promise.reject(new Error('非管理员组件无法调用 connect()'))
      }

  const safeDisconnect = owner
    ? state.disconnect
    : () => {
        // 非管理员组件无法调用 disconnect()
        if (import.meta.env.DEV) {
          console.warn('[useWebSocket] 非管理员组件尝试调用 disconnect()，已忽略。请使用 useWebSocket(true) 作为管理员。')
        }
      }

  return {
    isConnected: state.isConnected,
    isConnecting: state.isConnecting,
    connectionError: state.connectionError,
    videoFrame: state.videoFrame,
    videoFps: state.videoFps,
    odometry: state.odometry,
    availablePorts: state.availablePorts,
    bleDevices: state.bleDevices,
    status: state.status,
    linkStatus: state.linkStatus,
    connect: safeConnect,
    disconnect: safeDisconnect,
    sendCommand: state.sendCommand,
    sendSpeed: state.sendSpeed,
    sendDriveMode: state.sendDriveMode,
    sendMacConfig: state.sendMacConfig,
    sendBleScan: state.sendBleScan
  }
}
