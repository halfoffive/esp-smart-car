<template>
  <div class="panel flex flex-col gap-3 overflow-y-auto">
    <div class="panel-header">
      控制面板
    </div>
    
    <!-- 连接设置 -->
    <div class="flex gap-2 items-center">
      <select 
        v-model="selectedPort"
        class="flex-1 min-w-0 bg-dark-800 border border-dark-600 rounded-lg px-2 py-1.5 text-xs text-dark-100 focus:outline-none focus:border-primary-500"
      >
        <option value="">选择串口</option>
        <option v-for="port in availablePorts" :key="port" :value="port">
          {{ port }}
        </option>
      </select>
      
      <button 
        @click="isConnected ? disconnect() : connect()"
        :class="isConnected ? 'btn-danger' : 'btn-primary'"
        class="px-3 py-1.5 text-xs"
      >
        {{ isConnected ? '断开' : '连接' }}
      </button>
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
          @mousedown="sendCommand('U')"
          @mouseup="sendCommand(' ')"
          class="control-key-sm"
          title="云台上"
        >
          ↑
        </button>
        <button 
          @mousedown="sendCommand('W')"
          @mouseup="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('W') }]"
          title="前进"
        >
          W
        </button>
        <button 
          @mousedown="sendCommand('U')"
          @mouseup="sendCommand(' ')"
          class="control-key-sm"
          title="云台上"
        >
          ↑
        </button>
        
        <button 
          @mousedown="sendCommand('A')"
          @mouseup="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('A') }]"
          title="左转"
        >
          A
        </button>
        <button 
          @mousedown="sendCommand('S')"
          @mouseup="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('S') }]"
          title="后退"
        >
          S
        </button>
        <button 
          @mousedown="sendCommand('D')"
          @mouseup="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('D') }]"
          title="右转"
        >
          D
        </button>
        
        <button 
          @mousedown="sendCommand('Q')"
          @mouseup="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('Q') }]"
          title="原地左转"
        >
          Q
        </button>
        <button 
          @mousedown="sendCommand(' ')"
          @mouseup="sendCommand(' ')"
          class="control-key-sm text-red-400"
          title="停止"
        >
          ■
        </button>
        <button 
          @mousedown="sendCommand('E')"
          @mouseup="sendCommand(' ')"
          :class="['control-key-sm', { 'control-key-active': activeKeys.has('E') }]"
          title="原地右转"
        >
          E
        </button>
      </div>
    </div>
    
    <!-- 云台控制 -->
    <div>
      <h3 class="text-xs font-medium text-dark-300 mb-1.5">云台控制</h3>
      <div class="grid grid-cols-3 gap-1.5 max-w-[140px] mx-auto">
        <div></div>
        <button 
          @mousedown="sendCommand('U')"
          @mouseup="sendCommand(' ')"
          class="control-key-sm"
          title="上"
        >
          ↑
        </button>
        <div></div>
        
        <button 
          @mousedown="sendCommand('H')"
          @mouseup="sendCommand(' ')"
          class="control-key-sm"
          title="左"
        >
          ←
        </button>
        <button 
          @mousedown="sendCommand('C')"
          @mouseup="sendCommand(' ')"
          class="control-key-sm text-xs"
          title="居中"
        >
          C
        </button>
        <button 
          @mousedown="sendCommand('K')"
          @mouseup="sendCommand(' ')"
          class="control-key-sm"
          title="右"
        >
          →
        </button>
        
        <div></div>
        <button 
          @mousedown="sendCommand('J')"
          @mouseup="sendCommand(' ')"
          class="control-key-sm"
          title="下"
        >
          ↓
        </button>
        <div></div>
      </div>
    </div>
    
    <!-- 智能行走模式 -->
    <div>
      <div class="flex items-center justify-between mb-1.5">
        <h3 class="text-xs font-medium text-dark-300">智能修正</h3>
        <div class="flex items-center gap-1.5">
          <span class="text-[10px]" :class="smartDriveOn ? 'text-green-400' : 'text-dark-500'">
            {{ smartDriveOn ? 'ON' : 'OFF' }}
          </span>
          <button 
            @click="toggleSmartDrive"
            :class="[
              'relative inline-flex h-5 w-9 items-center rounded-full transition-colors',
              smartDriveOn ? 'bg-green-500' : 'bg-dark-600'
            ]"
          >
            <span 
              :class="[
                'inline-block h-3.5 w-3.5 transform rounded-full bg-white transition-transform',
                smartDriveOn ? 'translate-x-4.5' : 'translate-x-1'
              ]"
            ></span>
          </button>
        </div>
      </div>
      <p class="text-[9px] text-dark-600 leading-tight">
        启用后自动修正左右轮速度差，保持直线行走
      </p>
    </div>
    
    <!-- 紧急停止 -->
    <button 
      @click="emergencyStop"
      class="btn-danger w-full py-2 text-sm font-bold"
    >
      ⚠ 紧急停止
    </button>
    
    <!-- 系统日志 -->
    <div class="flex-1 min-h-0 flex flex-col">
      <h3 class="text-xs font-medium text-dark-300 mb-1">系统日志</h3>
      <div class="flex-1 bg-dark-950 rounded-lg p-2 overflow-y-auto font-mono text-[10px] space-y-0.5 min-h-[60px]">
        <div v-for="(log, index) in logs" :key="index" :class="log.color">
          <span class="text-dark-600">[{{ log.time }}]</span>
          {{ log.message }}
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useWebSocket } from '../composables/useWebSocket'
import { useKeyboard } from '../composables/useKeyboard'

const { sendCommand: wsSendCommand, connect: wsConnect, disconnect: wsDisconnect, sendDriveMode } = useWebSocket()
const { activeKeys, setupKeyboardListeners } = useKeyboard()

const selectedPort = ref('')
const availablePorts = ref<string[]>([])
const currentSpeed = ref(5)
const isConnected = ref(false)

/** 速度滑块防抖定时器：快速拖动时只发送最终值，不发送中间值 */
let speedDebounceTimer: number | null = null
const smartDriveOn = ref(false)
const logs = ref<{ time: string, message: string, color: string }[]>([])

const speedPercent = computed(() => Math.round((currentSpeed.value / 9) * 100))

const sliderBackground = computed(() => {
  const percent = ((currentSpeed.value - 1) / 8) * 100
  return `linear-gradient(to right, #0ea5e9 0%, #0ea5e9 ${percent}%, #374151 ${percent}%, #374151 100%)`
})

const toggleSmartDrive = () => {
  smartDriveOn.value = !smartDriveOn.value
  sendDriveMode(smartDriveOn.value ? 1 : 0)
  addLog(smartDriveOn.value ? '直线修正: 已启用' : '直线修正: 已禁用', 'info')
}

const addLog = (message: string, type: 'info' | 'warning' | 'error' = 'info') => {
  const colors = {
    info: 'text-dark-300',
    warning: 'text-yellow-400',
    error: 'text-red-400'
  }
  
  logs.value.unshift({
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
  addLog(`发送命令: ${cmd}`)
}

/** 速度滑块输入处理（带 200ms 防抖）：只发送最终值，不发送中间值 */
const handleSpeedInput = (event: Event) => {
  const target = event.target as HTMLInputElement
  currentSpeed.value = parseFloat(target.value)
  if (speedDebounceTimer !== null) {
    clearTimeout(speedDebounceTimer)
  }
  speedDebounceTimer = window.setTimeout(() => {
    speedDebounceTimer = null
    setSpeed()
  }, 200)
}

const setSpeed = () => {
  // 无极滑块值取整后发送给固件（固件只接受整数速度 1-9）
  const speed = Math.round(currentSpeed.value).toString()
  sendCommand(speed)
}

const connect = async () => {
  if (!selectedPort.value) {
    addLog('请选择串口', 'warning')
    return
  }
  
  try {
    const response = await fetch('/api/connect', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        port_name: selectedPort.value,
        baud_rate: 921600
      })
    })
    
    const result = await response.json()
    
    if (result.success) {
      isConnected.value = true
      wsConnect()
      addLog('串口连接成功', 'info')
    } else {
      addLog(`连接失败: ${result.message}`, 'error')
    }
  } catch (e) {
    addLog(`连接错误: ${e}`, 'error')
  }
}

const disconnect = async () => {
  try {
    const response = await fetch('/api/disconnect', { method: 'POST' })
    const result = await response.json()
    
    if (result.success) {
      isConnected.value = false
      wsDisconnect()
      addLog('串口已断开')
    }
  } catch (e) {
    addLog(`断开错误: ${e}`, 'error')
  }
}

const emergencyStop = () => {
  sendCommand(' ')
  addLog('紧急停止！', 'error')
}

const refreshPorts = async () => {
  try {
    const response = await fetch('/api/status')
    const status = await response.json()
    
    if (status.port_name) {
      availablePorts.value = [status.port_name]
    }
  } catch (e) {
    addLog('获取串口列表失败', 'warning')
  }
}

onMounted(() => {
  setupKeyboardListeners(sendCommand)
  refreshPorts()
})

onUnmounted(() => {
  if (isConnected.value) {
    disconnect()
  }
})
</script>