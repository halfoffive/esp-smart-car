<template>
  <div class="flex items-center gap-3 text-xs" role="status" aria-live="polite" aria-label="系统状态栏">
    <!-- WebSocket状态 -->
    <div class="flex items-center gap-1.5" aria-label="WebSocket连接状态">
      <span 
        :class="[
          'w-1.5 h-1.5 rounded-full',
          isConnected ? 'bg-green-500 animate-pulse' : 'bg-red-500'
        ]"
        aria-hidden="true"
      ></span>
      <span class="text-dark-400">
        WS {{ isConnected ? 'ON' : 'OFF' }}
      </span>
    </div>
    
    <!-- 串口状态 -->
    <div class="flex items-center gap-1.5" aria-label="串口连接状态">
      <span 
        :class="[
          'w-1.5 h-1.5 rounded-full',
          serialConnected ? 'bg-green-500' : 'bg-red-500'
        ]"
        aria-hidden="true"
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
import { computed } from 'vue'
import { useWebSocket } from '../composables/useWebSocket'
import { useStatus } from '../composables/useStatus'

const { isConnected, videoFps } = useWebSocket()
const { status } = useStatus()

const serialConnected = computed(() => status.value.serial_status === '已连接')
const fps = computed(() => videoFps.value || 0)
const currentSpeed = computed(() => Math.min(9, Math.max(1, status.value.current_speed || 5)))
const frameCount = computed(() => status.value.frame_count || 0)
</script>