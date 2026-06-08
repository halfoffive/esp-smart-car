<template>
  <div class="flex items-center gap-3 text-xs">
    <!-- WebSocket状态 -->
    <div class="flex items-center gap-1.5">
      <span 
        :class="[
          'w-1.5 h-1.5 rounded-full',
          isConnected ? 'bg-green-500 animate-pulse' : 'bg-red-500'
        ]"
      ></span>
      <span class="text-dark-400">
        WS {{ isConnected ? 'ON' : 'OFF' }}
      </span>
    </div>
    
    <!-- 串口状态 -->
    <div class="flex items-center gap-1.5">
      <span 
        :class="[
          'w-1.5 h-1.5 rounded-full',
          serialConnected ? 'bg-green-500' : 'bg-red-500'
        ]"
      ></span>
      <span class="text-dark-400">
        串口 {{ serialConnected ? 'ON' : 'OFF' }}
      </span>
    </div>
    
    <!-- 帧率 -->
    <div v-if="fps > 0" class="flex items-center gap-1 text-dark-400">
      <svg class="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z"/>
      </svg>
      <span class="font-mono">{{ fps }}FPS</span>
    </div>
    
    <!-- 当前速度 -->
    <div class="flex items-center gap-1 text-dark-400">
      <svg class="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 8v4l3 3m6-3a9 9 0 11-18 0 9 9 0 0118 0z"/>
      </svg>
      <span class="font-mono">{{ currentSpeed }}/9</span>
    </div>
    
    <!-- 帧数 -->
    <div class="flex items-center gap-1 text-dark-400">
      <svg class="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 16l4.586-4.586a2 2 0 012.828 0L16 16m-2-2l1.586-1.586a2 2 0 012.828 0L20 14m-6-6h.01M6 20h12a2 2 0 002-2V6a2 2 0 00-2-2H6a2 2 0 00-2 2v12a2 2 0 002 2z"/>
      </svg>
      <span class="font-mono">{{ frameCount }}</span>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import { useWebSocket } from '../composables/useWebSocket'

const { isConnected } = useWebSocket()
const serialConnected = ref(false)
const fps = ref(0)
const currentSpeed = ref(5)
const frameCount = ref(0)

let interval: number

const updateStatus = async () => {
  try {
    const response = await fetch('/api/status')
    const status = await response.json()
    
    serialConnected.value = status.serial_status === '已连接'
    fps.value = status.fps || 0
    // 将速度等级限制在 1-9 范围内，防止后端返回异常值
    currentSpeed.value = Math.min(9, Math.max(1, status.current_speed || 5))
    frameCount.value = status.frame_count || 0
  } catch {
    // 忽略错误
  }
}

onMounted(() => {
  updateStatus()
  interval = setInterval(updateStatus, 1000) as unknown as number
})

onUnmounted(() => {
  clearInterval(interval)
})
</script>