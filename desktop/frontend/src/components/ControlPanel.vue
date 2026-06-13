<template>
  <div class="panel flex flex-col gap-3 overflow-y-auto" role="region" aria-label="控制面板">
    <div class="panel-header">
      控制面板
    </div>
    
    <!-- 连接设置 -->
    <div class="flex flex-col gap-2">
      <div class="flex gap-2 items-center">
        <select
          v-model="selectedPort"
          aria-label="串口选择"
          class="flex-1 min-w-0 bg-dark-800 border border-dark-600 rounded-lg px-2 py-1.5 text-xs text-dark-100 focus:outline-none focus:border-primary-500"
        >
          <option value="">选择串口</option>
          <option v-for="port in wsAvailablePorts" :key="port" :value="port">
            {{ port }}
          </option>
        </select>

        <button
          @click="scanPorts"
          class="px-2 py-1.5 text-xs bg-dark-700 hover:bg-dark-600 text-dark-200 rounded-lg border border-dark-600 transition-colors shrink-0"
          :disabled="isScanning"
          :aria-label="isScanning ? '扫描中' : '扫描可用串口'"
        >
          {{ isScanning ? '扫描中...' : '扫描' }}
        </button>

        <button
          @click="serialConnected ? disconnect() : connect()"
          :class="[
            serialConnected ? 'btn-danger' : 'btn-primary',
            { 'opacity-50 cursor-not-allowed': isConnecting || serialConnecting }
          ]"
          class="px-3 py-1.5 text-xs shrink-0"
          :disabled="isConnecting || serialConnecting"
          :aria-label="isConnecting || serialConnecting ? '连接中' : (serialConnected ? '断开串口连接' : '连接串口')"
        >
          {{ isConnecting || serialConnecting ? '连接中...' : (serialConnected ? '断开' : '连接') }}
        </button>
      </div>


    </div>
    
    <!-- 速度控制 -->
    <div>
      <div class="flex items-center justify-between mb-1">
        <h3 class="text-xs font-medium text-dark-300">速度控制</h3>
        <span class="text-sm text-primary-400 font-mono font-bold">{{ speedPercent }}%</span>
      </div>
      
      <!-- 无极速度滑块 -->
      <div class="flex items-center gap-2">
        <span class="text-[10px] text-dark-500 font-mono w-3 text-center shrink-0">1</span>
        <div class="flex-1 relative">
          <input
            v-model.number="currentSpeed"
            type="range"
            min="1"
            max="9"
            step="0.1"
            class="speed-slider w-full"
            :style="{ background: sliderBackground }"
            @input="handleSpeedInput"
            aria-label="速度控制滑块"
          />
        </div>
        <span class="text-[10px] text-dark-500 font-mono w-3 text-center shrink-0">9</span>
      </div>
    </div>
    
    <!-- 运动控制 (WASD) -->
    <div>
      <h3 class="text-xs font-medium text-dark-300 mb-1.5">运动控制</h3>
      <div class="grid grid-cols-3 gap-1.5 max-w-[140px] mx-auto">
        <button
          @mousedown="sendCommand('Q')"
          @mouseup="sendCommand(' ')"
          @mouseleave="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('Q') }]"
          title="原地左转"
          aria-label="原地左转"
        >
          Q
        </button>
        <button
          @mousedown="sendCommand('W')"
          @mouseup="sendCommand(' ')"
          @mouseleave="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('W') }]"
          title="前进"
          aria-label="前进"
        >
          W
        </button>
        <button
          @mousedown="sendCommand('E')"
          @mouseup="sendCommand(' ')"
          @mouseleave="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('E') }]"
          title="原地右转"
          aria-label="原地右转"
        >
          E
        </button>

        <button
          @mousedown="sendCommand('A')"
          @mouseup="sendCommand(' ')"
          @mouseleave="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('A') }]"
          title="左转"
          aria-label="左转"
        >
          A
        </button>
        <button
          @mousedown="sendCommand('S')"
          @mouseup="sendCommand(' ')"
          @mouseleave="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('S') }]"
          title="后退"
          aria-label="后退"
        >
          S
        </button>
        <button
          @mousedown="sendCommand('D')"
          @mouseup="sendCommand(' ')"
          @mouseleave="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('D') }]"
          title="右转"
          aria-label="右转"
        >
          D
        </button>
        
      </div>
    </div>
    
    <!-- 行走模式选择 -->
    <div>
      <div class="flex items-center justify-between mb-1.5">
        <h3 class="text-xs font-medium text-dark-300">行走模式</h3>
        <div class="flex items-center gap-1">
          <button
            @click="setDriveMode(0)"
            :class="[
              'px-2 py-0.5 text-[10px] rounded transition-colors',
              driveMode === 0 ? 'bg-primary-500 text-white' : 'bg-dark-700 text-dark-400 hover:bg-dark-600'
            ]"
            aria-label="普通模式"
          >普通</button>
          <button
            @click="setDriveMode(1)"
            :class="[
              'px-2 py-0.5 text-[10px] rounded transition-colors',
              driveMode === 1 ? 'bg-green-500 text-white' : 'bg-dark-700 text-dark-400 hover:bg-dark-600'
            ]"
            aria-label="直线修正模式"
          >直线</button>
          <button
            @click="setDriveMode(2)"
            :class="[
              'px-2 py-0.5 text-[10px] rounded transition-colors',
              driveMode === 2 ? 'bg-cyan-500 text-white' : 'bg-dark-700 text-dark-400 hover:bg-dark-600'
            ]"
            aria-label="航向锁定模式"
          >锁定</button>
        </div>
      </div>
      <p class="text-[9px] text-dark-600 leading-tight">
        {{ driveModeDesc }}
      </p>
    </div>

    <!-- 蓝牙设备扫描 -->
    <div>
      <div class="flex items-center justify-between mb-1.5">
        <h3 class="text-xs font-medium text-dark-300">蓝牙设备</h3>
        <button
          @click="scanBleDevices"
          class="px-2 py-0.5 text-[10px] bg-dark-700 hover:bg-dark-600 text-dark-200 rounded border border-dark-600 transition-colors"
          :disabled="isBleScanning || !isConnected"
          :aria-label="isBleScanning ? '扫描中' : '扫描蓝牙设备'"
        >
          {{ isBleScanning ? '扫描中...' : '扫描' }}
        </button>
      </div>
      <div v-if="bleDevices.length > 0" class="space-y-1 max-h-[120px] overflow-y-auto">
        <div
          v-for="device in bleDevices"
          :key="device.mac"
          class="flex items-center justify-between bg-dark-800 rounded px-2 py-1 text-[10px]"
        >
          <span class="text-dark-200 truncate flex-1 min-w-0">{{ device.name }}</span>
          <span class="text-dark-500 font-mono ml-2 shrink-0">{{ device.mac }}</span>
          <span :class="device.rssi > -50 ? 'text-green-400' : device.rssi > -70 ? 'text-yellow-400' : 'text-red-400'" class="ml-2 shrink-0">
            {{ device.rssi }}dBm
          </span>
        </div>
      </div>
      <p v-else class="text-[9px] text-dark-600">点击扫描发现周围蓝牙设备</p>
    </div>

    <!-- 紧急停止 -->
    <button 
      @click="emergencyStop"
      class="btn-danger w-full py-2 text-sm font-bold"
      aria-label="紧急停止所有运动"
    >
      ⚠ 紧急停止
    </button>
    
    <!-- 系统日志 -->
    <div class="flex-1 min-h-0 flex flex-col">
      <h3 class="text-xs font-medium text-dark-300 mb-1">系统日志</h3>
      <div class="flex-1 bg-dark-950 rounded-lg p-2 overflow-y-auto font-mono text-[10px] space-y-0.5 min-h-[60px]" role="log" aria-label="系统日志" aria-live="polite">
        <div v-for="(log, index) in logs" :key="log.id ?? index" :class="log.color">
          <span class="text-dark-600">[{{ log.time }}]</span>
          {{ log.message }}
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import { useWebSocket } from '../composables/useWebSocket'
import { useKeyboard } from '../composables/useKeyboard'
import { useApi } from '../composables/useApi'
import { useStatus } from '../composables/useStatus'

const { sendCommand: wsSendCommand, sendSpeed, isConnected, sendDriveMode, availablePorts: wsAvailablePorts, connect: wsConnect, disconnect: wsDisconnect, bleDevices, sendBleScan } = useWebSocket(true)
const { post, get } = useApi()
const { status } = useStatus()

const selectedPort = ref('')
const currentSpeed = ref(5)
/** 连接进行中状态标志 */
const isConnecting = ref(false)
/** 串口是否已连接（基于后端 /api/status 轮询，解决 WebSocket 断开时按钮状态不一致） */
const serialConnected = computed(() => status.value.serial_status === '已连接')
/** 串口是否正在连接中 */
const serialConnecting = computed(() => status.value.serial_status === '连接中')
/** 串口扫描进行中状态标志 */
const isScanning = ref(false)

/** 速度滑块防抖定时器：快速拖动时只发送最终值，不发送中间值 */
let speedDebounceTimer: number | null = null
/** 行走模式：0=普通, 1=直线修正, 2=航向锁定 */
const driveMode = ref(0)
const logs = ref<{ id: number, time: string, message: string, color: string }[]>([])

/** BLE 扫描进行中状态 */
const isBleScanning = ref(false)

/** 扫描蓝牙设备 */
const scanBleDevices = async () => {
  if (!isConnected.value) {
    addLog('未连接，无法扫描蓝牙设备', 'warning')
    return
  }
  isBleScanning.value = true
  const success = sendBleScan()
  if (!success) {
    addLog('蓝牙扫描命令发送失败', 'error')
  } else {
    addLog('蓝牙扫描已触发，等待结果...', 'info')
  }
  // BLE 扫描约 10 秒，12 秒后自动取消扫描状态
  setTimeout(() => {
    isBleScanning.value = false
  }, 12000)
}
const speedPercent = computed(() => Math.round((currentSpeed.value / 9) * 100))

const sliderBackground = computed(() => {
  const percent = ((currentSpeed.value - 1) / 8) * 100
  return `linear-gradient(to right, #0ea5e9 0%, #0ea5e9 ${percent}%, #374151 ${percent}%, #374151 100%)`
})

/** 行走模式描述文本 */
const driveModeDesc = computed(() => {
  switch (driveMode.value) {
    case 0: return '普通模式：无自动修正'
    case 1: return '直线修正：自动修正左右轮速度差'
    case 2: return '航向锁定：锁定当前航向角，自动纠偏'
    default: return ''
  }
})

/** 设置行走模式：0=普通, 1=直线修正, 2=航向锁定 */
const setDriveMode = (mode: number) => {
  if (!isConnected.value) {
    addLog('未连接，无法切换模式', 'warning')
    return
  }
  driveMode.value = mode
  sendDriveMode(mode)
  addLog(`行走模式: ${driveModeDesc.value}`, 'info')
}

const addLog = (message: string, type: 'info' | 'warning' | 'error' = 'info') => {
  const colors = {
    info: 'text-dark-300',
    warning: 'text-yellow-400',
    error: 'text-red-400'
  }
  
  logs.value.unshift({
    id: Date.now(),
    time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
    message,
    color: colors[type]
  })
  
  if (logs.value.length > 30) {
    logs.value.pop()
  }
}

const sendCommand = (cmd: string) => {
  if (!isConnected.value) {
    addLog('未连接，无法发送命令', 'warning')
    return
  }
  
  wsSendCommand(cmd)
  // 注：高频命令发送不记录日志，避免日志洪流
}

// 使用重构后的 useKeyboard：自动管理生命周期，无需手动清理
const { activeKeys } = useKeyboard(sendCommand)

/** 速度滑块输入处理（带 200ms 防抖）：只发送最终值，不发送中间值 */
const handleSpeedInput = () => {
  if (speedDebounceTimer !== null) {
    clearTimeout(speedDebounceTimer)
  }
  speedDebounceTimer = window.setTimeout(() => {
    speedDebounceTimer = null
    setSpeed()
  }, 200)
}

const setSpeed = () => {
  if (!isConnected.value) {
    addLog('WebSocket 未连接，无法设置速度', 'warning')
    return
  }
  const speed = Math.round(currentSpeed.value)
  sendSpeed(speed)
}

const connect = async () => {
  if (!selectedPort.value) {
    addLog('请选择串口', 'warning')
    return
  }
  
  isConnecting.value = true
  
  try {
    const result = await post('/api/connect', {
      port_name: selectedPort.value,
      baud_rate: 921600
    })
    
    if (result.success) {
      addLog('串口连接成功', 'info')
      // 串口连接成功后自动连接 WebSocket（await 确保异常可被 catch 捕获）
      try {
        await wsConnect()
      } catch (e) {
        addLog(`WebSocket 连接失败: ${e instanceof Error ? e.message : String(e)}`, 'warning')
        // WebSocket 失败时回滚串口连接，避免串口打开但 WS 断开的不一致状态
        await post('/api/disconnect').catch(() => {})
      }
    } else {
      addLog(`连接失败: ${result.message}`, 'error')
    }
  } catch (e) {
    addLog(`连接错误: ${e instanceof Error ? e.message : String(e)}`, 'error')
  } finally {
    isConnecting.value = false
  }
}

const disconnect = async () => {
  try {
    const result = await post('/api/disconnect')
    
    if (result.success) {
      addLog('串口已断开')
    }
  } catch (e) {
    addLog(`断开错误: ${e instanceof Error ? e.message : String(e)}`, 'error')
  }
  // 串口断开时同时断开 WebSocket
  wsDisconnect()
}

const emergencyStop = () => {
  sendCommand(' ')
  addLog('紧急停止！', 'error')
}

/** 扫描可用串口：调用 /api/ports 获取列表并填充下拉框（兜底手动扫描） */
const scanPorts = async () => {
  isScanning.value = true
  try {
    const result = await get<{ success: boolean; ports: string[] }>('/api/ports')

    if (result.success && result.ports.length > 0) {
      addLog(`发现 ${result.ports.length} 个串口: ${result.ports.join(', ')}`, 'info')
    } else {
      addLog('未找到可用串口', 'warning')
    }
  } catch (e) {
    addLog(`扫描串口失败: ${e instanceof Error ? e.message : String(e)}`, 'error')
  } finally {
    isScanning.value = false
  }
}

onMounted(() => {
  // 页面加载时无需恢复 MAC 地址（已移除 MAC 地址设置功能）
})

/** 当 WebSocket 推送的可用串口列表变化时，如果当前选中的串口已不在列表中则清除 */
watch(wsAvailablePorts, (newPorts) => {
  if (selectedPort.value && !newPorts.includes(selectedPort.value)) {
    selectedPort.value = ''
  }
})

onUnmounted(() => {
  // 清理速度防抖定时器
  if (speedDebounceTimer !== null) {
    clearTimeout(speedDebounceTimer)
    speedDebounceTimer = null
  }
  // 断开连接
  if (serialConnected.value) {
    disconnect().catch((e) => { if (import.meta.env.DEV) console.error('[ControlPanel] 卸载断开失败:', e) })
  }
})
</script>
