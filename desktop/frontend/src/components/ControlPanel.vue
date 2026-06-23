<template>
  <div class="panel flex flex-col gap-3 overflow-y-auto" role="region" aria-label="控制面板">
    <div class="panel-header">
      控制面板
    </div>
    
    <!-- 连接设置（后端不可用时隐藏） -->
    <div v-if="backendAvailable" class="flex flex-col gap-2">
      <div class="flex gap-2 items-center">
        <select
          v-model="selectedPort"
          aria-label="串口选择"
          class="flex-1 min-w-0 bg-dark-800 border border-dark-600 rounded-lg px-2 py-1.5 text-xs text-dark-100 focus:outline-none focus:border-primary-500"
        >
          <option value="">选择串口</option>
          <option v-for="port in displayedPorts" :key="port" :value="port">
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
            { 'opacity-50 cursor-not-allowed': serialConnecting }
          ]"
          class="px-3 py-1.5 text-xs shrink-0"
          :disabled="serialConnecting"
          :aria-label="serialConnecting ? '连接中' : (serialConnected ? '断开串口连接' : '连接串口')"
        >
          {{ serialConnecting ? '连接中...' : (serialConnected ? '断开' : '连接') }}
        </button>
      </div>

      <!-- 连接错误提示（WebSocket 或串口） -->
      <div v-if="connectionError" class="text-[10px] text-red-400 bg-red-400/10 border border-red-400/30 rounded px-2 py-1">
        {{ connectionError }}
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
        <span class="text-[10px] text-dark-500 font-mono w-5 text-center shrink-0">0</span>
        <div class="flex-1 relative">
          <input
            v-model.number="currentSpeed"
            type="range"
            min="0"
            max="255"
            step="1"
            class="speed-slider w-full"
            :style="{ background: sliderBackground }"
            @input="handleSpeedInput"
            @mousedown="isDraggingSpeed = true"
            @mouseup="isDraggingSpeed = false"
            @mouseleave="isDraggingSpeed = false"
            @touchstart="isDraggingSpeed = true"
            @touchend="isDraggingSpeed = false"
            :disabled="!backendAvailable"
            aria-label="速度控制滑块"
          />
        </div>
        <span class="text-[10px] text-dark-500 font-mono w-5 text-center shrink-0">255</span>
      </div>
    </div>
    
    <!-- 运动控制 (WASD) -->
    <div>
      <h3 class="text-xs font-medium text-dark-300 mb-1.5">运动控制</h3>
      <div class="grid grid-cols-3 gap-1.5 max-w-[140px] mx-auto">
        <button
          @mousedown="handleButtonPress('Q')"
          @mouseup="handleButtonRelease()"
          @mouseleave="handleButtonRelease()"
          @touchstart.prevent="handleButtonPress('Q')"
          @touchend.prevent="handleButtonRelease()"
          @touchcancel.prevent="handleButtonRelease()"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('Q') || pressedButton === 'Q' }]"
          :disabled="!backendAvailable"
          title="原地左转"
          aria-label="原地左转"
        >
          Q
        </button>
        <button
          @mousedown="handleButtonPress('W')"
          @mouseup="handleButtonRelease()"
          @mouseleave="handleButtonRelease()"
          @touchstart.prevent="handleButtonPress('W')"
          @touchend.prevent="handleButtonRelease()"
          @touchcancel.prevent="handleButtonRelease()"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('W') || pressedButton === 'W' }]"
          :disabled="!backendAvailable"
          title="前进"
          aria-label="前进"
        >
          W
        </button>
        <button
          @mousedown="handleButtonPress('E')"
          @mouseup="handleButtonRelease()"
          @mouseleave="handleButtonRelease()"
          @touchstart.prevent="handleButtonPress('E')"
          @touchend.prevent="handleButtonRelease()"
          @touchcancel.prevent="handleButtonRelease()"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('E') || pressedButton === 'E' }]"
          :disabled="!backendAvailable"
          title="原地右转"
          aria-label="原地右转"
        >
          E
        </button>

        <button
          @mousedown="handleButtonPress('A')"
          @mouseup="handleButtonRelease()"
          @mouseleave="handleButtonRelease()"
          @touchstart.prevent="handleButtonPress('A')"
          @touchend.prevent="handleButtonRelease()"
          @touchcancel.prevent="handleButtonRelease()"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('A') || pressedButton === 'A' }]"
          :disabled="!backendAvailable"
          title="左转"
          aria-label="左转"
        >
          A
        </button>
        <button
          @mousedown="handleButtonPress('S')"
          @mouseup="handleButtonRelease()"
          @mouseleave="handleButtonRelease()"
          @touchstart.prevent="handleButtonPress('S')"
          @touchend.prevent="handleButtonRelease()"
          @touchcancel.prevent="handleButtonRelease()"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('S') || pressedButton === 'S' }]"
          :disabled="!backendAvailable"
          title="后退"
          aria-label="后退"
        >
          S
        </button>
        <button
          @mousedown="handleButtonPress('D')"
          @mouseup="handleButtonRelease()"
          @mouseleave="handleButtonRelease()"
          @touchstart.prevent="handleButtonPress('D')"
          @touchend.prevent="handleButtonRelease()"
          @touchcancel.prevent="handleButtonRelease()"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('D') || pressedButton === 'D' }]"
          :disabled="!backendAvailable"
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
            :disabled="!backendAvailable"
            aria-label="普通模式"
          >普通</button>
          <button
            @click="setDriveMode(1)"
            :class="[
              'px-2 py-0.5 text-[10px] rounded transition-colors',
              driveMode === 1 ? 'bg-green-500 text-white' : 'bg-dark-700 text-dark-400 hover:bg-dark-600'
            ]"
            :disabled="!backendAvailable"
            aria-label="直线修正模式"
          >直线</button>
          <button
            @click="setDriveMode(2)"
            :class="[
              'px-2 py-0.5 text-[10px] rounded transition-colors',
              driveMode === 2 ? 'bg-cyan-500 text-white' : 'bg-dark-700 text-dark-400 hover:bg-dark-600'
            ]"
            :disabled="!backendAvailable"
            aria-label="航向锁定模式"
          >锁定</button>
        </div>
      </div>
      <p class="text-[9px] text-dark-600 leading-tight">
        {{ driveModeDesc }}
      </p>
    </div>

    <!-- 系统日志 -->
    <div class="flex-1 min-h-0 flex flex-col">
      <h3 class="text-xs font-medium text-dark-300 mb-1">系统日志</h3>
      <div class="flex-1 bg-dark-950 rounded-lg p-2 overflow-y-auto font-mono text-[10px] space-y-0.5 min-h-[60px]" role="log" aria-label="系统日志" aria-live="polite">
        <div v-for="log in logs" :key="log.id" :class="log.color">
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
import { useBackendHealth } from '../composables/useBackendHealth'
import { COMMAND_REPEAT_INTERVAL_MS } from '../config/constants'

const { sendCommand: wsSendCommand, sendSpeed, isConnected, connectionError, sendDriveMode, availablePorts: wsAvailablePorts } = useWebSocket()
const { post, get } = useApi()
const { status } = useStatus()
const { backendAvailable } = useBackendHealth()

const selectedPort = ref('')
const scannedPorts = ref<string[]>([])
const displayedPorts = computed(() => {
  const merged = new Set([...scannedPorts.value, ...wsAvailablePorts.value])
  return [...merged].sort()
})
const currentSpeed = ref(128)
const isDraggingSpeed = ref(false)
const serialConnected = computed(() => status.value.serialStatus.startsWith('已连接'))
const serialConnecting = computed(() => status.value.serialStatus === '连接中')
const isScanning = ref(false)

let speedDebounceTimer: number | null = null
let buttonRepeatTimer: ReturnType<typeof setInterval> | null = null
const pressedButton = ref<string | null>(null)
const driveMode = computed(() => status.value.driveMode ?? 0)
const logs = ref<{ id: number, time: string, message: string, color: string }[]>([])
/** 日志 ID 自增计数器（避免 Date.now() 碰撞） */
let logIdCounter = 0

const speedPercent = computed(() => Math.round((currentSpeed.value / 255) * 100))

const sliderBackground = computed(() => {
  const percent = (currentSpeed.value / 255) * 100
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

const setDriveMode = (mode: number) => {
  if (!isConnected.value) {
    addLog('未连接，无法切换模式', 'warning')
    return
  }
  if (sendDriveMode(mode)) {
    addLog(`行走模式: ${driveModeDesc.value}`, 'info')
  } else {
    addLog('行走模式切换失败，请检查 WebSocket 连接', 'error')
  }
}

const addLog = (message: string, type: 'info' | 'warning' | 'error' = 'info') => {
  const colors = {
    info: 'text-dark-300',
    warning: 'text-yellow-400',
    error: 'text-red-400'
  }

  logs.value.unshift({
    id: ++logIdCounter,
    time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
    message,
    color: colors[type]
  })

  if (logs.value.length > 30) {
    logs.value.pop()
  }
}

const sendCommand = (cmd: string) => {
  if (!backendAvailable.value) return
  if (!isConnected.value) {
    addLog('未连接，无法发送命令', 'warning')
    return
  }

  wsSendCommand(cmd)
}

const stopButtonRepeat = () => {
  if (buttonRepeatTimer) {
    clearInterval(buttonRepeatTimer)
    buttonRepeatTimer = null
  }
}

const handleButtonPress = (cmd: string) => {
  if (!backendAvailable.value) return
  if (!isConnected.value) {
    addLog('未连接，无法发送命令', 'warning')
    return
  }
  stopButtonRepeat()
  pressedButton.value = cmd
  sendCommand(cmd)
  buttonRepeatTimer = setInterval(() => {
    sendCommand(cmd)
  }, COMMAND_REPEAT_INTERVAL_MS)
}

const DIRECTION_KEYS = ['W', 'A', 'S', 'D', 'Q', 'E']

const handleButtonRelease = () => {
  if (!pressedButton.value) return
  stopButtonRepeat()
  pressedButton.value = null

  const remainingDirection = DIRECTION_KEYS.find((key) => activeKeys.has(key))
  if (remainingDirection) {
    sendCommand(remainingDirection)
  } else {
    sendCommand(' ')
  }
}

/** 速度滑块输入处理（带 200ms 防抖）：只发送最终值，不发送中间值 */
const handleSpeedInput = () => {
  if (speedDebounceTimer !== null) {
    clearTimeout(speedDebounceTimer)
  }
  speedDebounceTimer = window.setTimeout(() => {
    speedDebounceTimer = null
    setSpeed(currentSpeed.value)
  }, 200)
}

const setSpeed = (pwm: number) => {
  if (!backendAvailable.value) return
  if (!isConnected.value) {
    addLog('WebSocket 未连接，无法设置速度', 'warning')
    return
  }
  sendSpeed(Math.round(pwm))
}

const { activeKeys } = useKeyboard(sendCommand, setSpeed)

// 速度滑块与后端 currentSpeed 同步（仅在未拖动时）
watch(() => status.value.currentSpeed, (v) => {
  if (!isDraggingSpeed.value && typeof v === 'number') {
    currentSpeed.value = v
  }
})

const connect = async () => {
  if (!selectedPort.value) {
    addLog('请选择串口', 'warning')
    return
  }

  try {
    const result = await post('/api/connect', {
      port_name: selectedPort.value,
      baud_rate: 3000000
    })

    if (result.success) {
      addLog('串口连接成功', 'info')
    } else {
      addLog(`连接失败: ${result.message}`, 'error')
    }
  } catch (e) {
    addLog(`连接错误: ${e instanceof Error ? e.message : String(e)}`, 'error')
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
}

/** 扫描可用串口：调用 /api/ports 获取列表并填充下拉框（兜底手动扫描） */
const scanPorts = async () => {
  isScanning.value = true
  try {
    const result = await get<{ success: boolean; ports: string[] }>('/api/ports')

    if (result.success && result.ports.length > 0) {
      scannedPorts.value = result.ports
      addLog(`发现 ${result.ports.length} 个串口: ${result.ports.join(', ')}`, 'info')
    } else {
      scannedPorts.value = []
      addLog('未找到可用串口', 'warning')
    }
  } catch (e) {
    scannedPorts.value = []
    addLog(`扫描串口失败: ${e instanceof Error ? e.message : String(e)}`, 'error')
  } finally {
    isScanning.value = false
  }
}

/** 当可用串口列表变化时，如果当前选中的串口已不在列表中则清除 */
watch(displayedPorts, (newPorts) => {
  if (selectedPort.value && !newPorts.includes(selectedPort.value)) {
    selectedPort.value = ''
  }
})

onMounted(() => {
  scanPorts()
})

onUnmounted(() => {
  if (speedDebounceTimer !== null) {
    clearTimeout(speedDebounceTimer)
    speedDebounceTimer = null
  }
  stopButtonRepeat()
})
</script>
