<template>
  <div class="panel flex flex-col gap-4">
    <div class="panel-header">
      控制面板
    </div>
    
    <!-- 连接设置 -->
    <div class="space-y-3">
      <h3 class="text-sm font-medium text-dark-300">串口连接</h3>
      
      <div class="flex gap-2">
        <select 
          v-model="selectedPort"
          class="flex-1 bg-dark-800 border border-dark-600 rounded-lg px-3 py-2 text-sm text-dark-100 focus:outline-none focus:border-primary-500"
        >
          <option value="">选择串口</option>
          <option v-for="port in availablePorts" :key="port" :value="port">
            {{ port }}
          </option>
        </select>
        
        <button 
          @click="isConnected ? disconnect() : connect()"
          :class="isConnected ? 'btn-danger' : 'btn-primary'"
          class="px-4"
        >
          {{ isConnected ? '断开' : '连接' }}
        </button>
      </div>
    </div>
    
    <!-- 速度控制 -->
    <div class="space-y-3">
      <h3 class="text-sm font-medium text-dark-300">
        速度控制
        <span class="text-primary-400 font-mono ml-2">{{ currentSpeed }}%</span>
      </h3>
      
      <div class="flex items-center gap-3">
        <span class="text-xs text-dark-500">1</span>
        <input 
          v-model.number="currentSpeed"
          type="range"
          min="1"
          max="9"
          class="flex-1 h-2 bg-dark-700 rounded-lg appearance-none cursor-pointer accent-primary-500"
          @change="setSpeed"
        />
        <span class="text-xs text-dark-500">9</span>
      </div>
    </div>
    
    <!-- WASD 控制 -->
    <div class="space-y-3">
      <h3 class="text-sm font-medium text-dark-300">运动控制</h3>
      
      <div class="grid grid-cols-3 gap-2 max-w-[180px] mx-auto">
        <!-- 第一行：云台控制 -->
        <button 
          @mousedown="sendCommand('U')"
          @mouseup="sendCommand(' ')"
          class="control-key"
          title="云台上"
        >
          <svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 15l7-7 7 7"/>
          </svg>
        </button>
        <button 
          @mousedown="sendCommand('W')"
          @mouseup="sendCommand(' ')"
          :class="['control-key', { active: activeKeys.has('W') }]"
          title="前进"
        >
          W
        </button>
        <button 
          @mousedown="sendCommand('U')"
          @mouseup="sendCommand(' ')"
          class="control-key"
          title="云台上"
        >
          <svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 15l7-7 7 7"/>
          </svg>
        </button>
        
        <!-- 第二行 -->
        <button 
          @mousedown="sendCommand('A')"
          @mouseup="sendCommand(' ')"
          :class="['control-key', { active: activeKeys.has('A') }]"
          title="左转"
        >
          A
        </button>
        <button 
          @mousedown="sendCommand('S')"
          @mouseup="sendCommand(' ')"
          :class="['control-key', { active: activeKeys.has('S') }]"
          title="后退"
        >
          S
        </button>
        <button 
          @mousedown="sendCommand('D')"
          @mouseup="sendCommand(' ')"
          :class="['control-key', { active: activeKeys.has('D') }]"
          title="右转"
        >
          D
        </button>
        
        <!-- 第三行 -->
        <button 
          @mousedown="sendCommand('Q')"
          @mouseup="sendCommand(' ')"
          class="control-key text-sm"
          title="原地左转"
        >
          Q
        </button>
        <button 
          @mousedown="sendCommand(' ')"
          @mouseup="sendCommand(' ')"
          class="control-key"
          title="停止"
        >
          <svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <rect x="6" y="6" width="12" height="12" rx="2" stroke-width="2"/>
          </svg>
        </button>
        <button 
          @mousedown="sendCommand('E')"
          @mouseup="sendCommand(' ')"
          class="control-key text-sm"
          title="原地右转"
        >
          E
        </button>
      </div>
      
      <p class="text-xs text-dark-500 text-center mt-2">
        也可以使用键盘 WASD 控制
      </p>
    </div>
    
    <!-- 云台控制 -->
    <div class="space-y-3">
      <h3 class="text-sm font-medium text-dark-300">云台控制</h3>
      
      <div class="grid grid-cols-3 gap-2 max-w-[180px] mx-auto">
        <div></div>
        <button 
          @mousedown="sendCommand('U')"
          @mouseup="sendCommand(' ')"
          class="control-key"
          title="上"
        >
          <svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 15l7-7 7 7"/>
          </svg>
        </button>
        <div></div>
        
        <button 
          @mousedown="sendCommand('L')"
          @mouseup="sendCommand(' ')"
          class="control-key"
          title="左"
        >
          <svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 19l-7-7 7-7"/>
          </svg>
        </button>
        <button 
          @mousedown="sendCommand('C')"
          @mouseup="sendCommand(' ')"
          class="control-key text-sm"
          title="居中"
        >
          C
        </button>
        <button 
          @mousedown="sendCommand('R')"
          @mouseup="sendCommand(' ')"
          class="control-key"
          title="右"
        >
          <svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 5l7 7-7 7"/>
          </svg>
        </button>
        
        <div></div>
        <button 
          @mousedown="sendCommand('D')"
          @mouseup="sendCommand(' ')"
          class="control-key"
          title="下"
        >
          <svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/>
          </svg>
        </button>
        <div></div>
      </div>
    </div>
    
    <!-- 紧急停止 -->
    <button 
      @click="emergencyStop"
      class="btn-danger w-full py-3 text-lg font-bold"
    >
      <svg class="w-6 h-6 inline mr-2" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z"/>
      </svg>
      紧急停止
    </button>
    
    <!-- 日志输出 -->
    <div class="flex-1 min-h-0 flex flex-col">
      <h3 class="text-sm font-medium text-dark-300 mb-2">系统日志</h3>
      <div class="flex-1 bg-dark-950 rounded-lg p-2 overflow-y-auto font-mono text-xs space-y-1 max-h-32">
        <div v-for="(log, index) in logs" :key="index" :class="log.color">
          <span class="text-dark-600">[{{ log.time }}]</span>
          {{ log.message }}
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import { useWebSocket } from '../composables/useWebSocket'
import { useKeyboard } from '../composables/useKeyboard'

const { sendCommand: wsSendCommand, isConnected: wsConnected, connect: wsConnect, disconnect: wsDisconnect } = useWebSocket()
const { activeKeys, setupKeyboardListeners } = useKeyboard()

const selectedPort = ref('')
const availablePorts = ref<string[]>([])
const currentSpeed = ref(5)
const isConnected = ref(false)
const logs = ref<{ time: string, message: string, color: string }[]>([])

// 添加日志
const addLog = (message: string, type: 'info' | 'warning' | 'error' = 'info') => {
  const colors = {
    info: 'text-dark-300',
    warning: 'text-yellow-400',
    error: 'text-red-400'
  }
  
  logs.value.unshift({
    time: new Date().toLocaleTimeString(),
    message,
    color: colors[type]
  })
  
  if (logs.value.length > 50) {
    logs.value.pop()
  }
}

// 发送命令
const sendCommand = (cmd: string) => {
  if (!isConnected.value) {
    addLog('未连接，无法发送命令', 'warning')
    return
  }
  
  wsSendCommand(cmd)
  addLog(`发送命令: ${cmd}`)
}

// 设置速度
const setSpeed = () => {
  const speed = currentSpeed.value.toString()
  sendCommand(speed)
}

// 连接串口
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

// 断开连接
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

// 紧急停止
const emergencyStop = () => {
  sendCommand(' ')
  addLog('紧急停止！', 'error')
}

// 获取可用串口
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
