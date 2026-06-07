<template>
  <div class="flex-1 flex flex-col min-h-screen">
    <!-- 顶部状态栏 -->
    <header class="bg-dark-900 border-b border-dark-700 px-6 py-3">
      <div class="flex items-center justify-between">
        <div class="flex items-center gap-4">
          <div class="flex items-center gap-2">
            <div class="w-8 h-8 bg-primary-600 rounded-lg flex items-center justify-center">
              <svg class="w-5 h-5 text-white" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" 
                      d="M13 10V3L4 14h7v7l9-11h-7z"/>
              </svg>
            </div>
            <h1 class="text-xl font-bold text-dark-100">智能车控制系统</h1>
          </div>
          <span class="text-xs text-dark-500 bg-dark-800 px-2 py-1 rounded-md">
            v1.0.0
          </span>
        </div>
        
        <StatusBar />
      </div>
    </header>
    
    <!-- 主内容区 -->
    <main class="flex-1 flex gap-4 p-4 overflow-hidden">
      <!-- 左侧视频区 -->
      <div class="flex-1 flex flex-col gap-4 min-w-0">
        <VideoPlayer class="flex-1" />
      </div>
      
      <!-- 右侧控制面板 -->
      <div class="w-80 flex flex-col gap-4">
        <ControlPanel />
      </div>
    </main>
    
    <!-- 底部信息栏 -->
    <footer class="bg-dark-900 border-t border-dark-700 px-6 py-2 text-sm text-dark-500">
      <div class="flex items-center justify-between">
        <span>ESP32 Smart Car Control System</span>
        <span class="flex items-center gap-2">
          <span class="w-2 h-2 rounded-full bg-green-500 animate-pulse"></span>
          系统运行中
        </span>
      </div>
    </footer>
  </div>
</template>

<script setup lang="ts">
import { onMounted } from 'vue'
import VideoPlayer from './components/VideoPlayer.vue'
import ControlPanel from './components/ControlPanel.vue'
import StatusBar from './components/StatusBar.vue'
import { useWebSocket } from './composables/useWebSocket'

const { connect } = useWebSocket()

onMounted(() => {
  // 自动连接WebSocket
  connect()
})
</script>
