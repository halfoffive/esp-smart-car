<template>
  <div class="flex-1 flex flex-col h-screen overflow-hidden">
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
            v1.2.0
          </span>
        </div>
        
        <StatusBar />
      </div>
    </header>
    
    <!-- 主内容区 -->
    <main class="flex-1 flex gap-3 p-3 overflow-hidden min-h-0">
      <!-- 左侧视频区 -->
      <div class="flex-1 flex flex-col gap-3 min-w-0 min-h-0">
        <VideoPlayer />
      </div>
      
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
import { onMounted } from 'vue'
import VideoPlayer from './components/VideoPlayer.vue'
import ControlPanel from './components/ControlPanel.vue'
import StatusBar from './components/StatusBar.vue'
import SpeedDashboard from './components/SpeedDashboard.vue'
import { useWebSocket } from './composables/useWebSocket'

const { connect } = useWebSocket(true)

onMounted(() => {
  connect()
})
</script>