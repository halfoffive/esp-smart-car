<template>
  <div class="flex-1 flex flex-col h-screen overflow-hidden">
    <!-- 后端不可用横幅（红色，固定顶部） -->
    <div
      v-if="!backendAvailable"
      class="bg-red-600 text-white text-center text-sm font-medium py-2 px-4 shrink-0"
      role="alert"
      aria-live="assertive"
    >
      ⚠ 后端未运行，请启动桌面端后端程序
    </div>

    <!-- 顶部状态栏 -->
    <header class="bg-dark-900 border-b border-dark-700 px-4 py-1.5 shrink-0">
      <div class="flex items-center justify-between">
        <div class="flex items-center gap-2">
          <div class="w-6 h-6 bg-primary-600 rounded-md flex items-center justify-center">
            <svg class="w-4 h-4 text-white" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                    d="M13 10V3L4 14h7v7l9-11h-7z"/>
            </svg>
          </div>
          <h1 class="text-base font-bold text-dark-100">智能车控制系统</h1>
          <span class="text-[10px] text-dark-500 bg-dark-800 px-1.5 py-0.5 rounded">
            v{{ appVersion }}
          </span>
        </div>

        <StatusBar />
      </div>
    </header>

    <!-- 主内容区 -->
    <main class="flex-1 flex gap-3 p-3 overflow-hidden min-h-0">
      <!-- 左侧视频区 -->
      <VideoPlayer class="flex-1 min-w-0 min-h-0" />

      <!-- 右侧控制面板 -->
      <div class="w-72 shrink-0 flex flex-col gap-3 min-h-0 overflow-y-auto">
        <ControlPanel />

        <!-- 4个测速模块 -->
        <SpeedDashboard />
      </div>
    </main>
  </div>
</template>

<script setup lang="ts">
import { onMounted, onUnmounted, watch } from 'vue'
import VideoPlayer from './components/VideoPlayer.vue'
import ControlPanel from './components/ControlPanel.vue'
import StatusBar from './components/StatusBar.vue'
import SpeedDashboard from './components/SpeedDashboard.vue'
import { useBackendHealth } from './composables/useBackendHealth'
import { useWebSocket } from './composables/useWebSocket'

const { backendAvailable, start: startHealthCheck, stop: stopHealthCheck } = useBackendHealth()
const { connect: wsConnect, disconnect: wsDisconnect, isConnected: wsConnected } = useWebSocket()

const appVersion = __APP_VERSION__

watch(backendAvailable, (available, wasAvailable) => {
  if (available && !wasAvailable && !wsConnected.value) {
    wsConnect().catch((err: Error) => {
      console.warn('[App] 后端恢复后自动重连失败:', err.message)
    })
  }
})

onMounted(() => {
  startHealthCheck()
  wsConnect().catch((err: Error) => {
    console.warn('[App] 初始 WebSocket 连接失败:', err.message)
  })
})

onUnmounted(() => {
  stopHealthCheck()
  wsDisconnect()
})
</script>
