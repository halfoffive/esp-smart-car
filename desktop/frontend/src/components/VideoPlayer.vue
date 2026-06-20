<template>
  <div class="panel flex-1 flex flex-col min-h-0" role="region" aria-label="实时视频区域">
    <div class="panel-header flex items-center justify-between py-2">
      <span class="text-sm">实时视频</span>
      <div class="flex items-center gap-2">
        <span v-if="fps > 0" class="text-xs text-primary-400 font-mono">
          {{ fps }} FPS
        </span>
      </div>
    </div>
    
    <div class="video-container flex-1 min-h-0 relative" aria-label="视频画面">
      <img 
        v-if="videoSrc"
        :src="videoSrc"
        alt="智能车实时摄像头画面"
        class="w-full h-full object-contain"
      />
      
      <div v-else class="video-placeholder">
        <div class="text-center">
          <svg class="w-12 h-12 mx-auto mb-3 text-dark-600" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5" 
                  d="M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z"/>
          </svg>
          <p class="text-sm text-dark-400">等待视频信号</p>
          <p class="text-xs text-dark-600 mt-1">请确保摄像头模块已连接</p>
        </div>
      </div>
      
      
      <div class="absolute top-2 right-2" role="status" aria-live="polite">
        <span v-if="isConnected" class="status-online text-xs">
          <span class="w-1.5 h-1.5 rounded-full bg-green-400 animate-pulse"></span>
          已连接
        </span>
        <span v-else class="status-offline text-xs">
          <span class="w-1.5 h-1.5 rounded-full bg-red-400"></span>
          未连接
        </span>
      </div>
    </div>
    
    <div class="flex items-center gap-2 mt-2">
      <button 
        @click="takeSnapshot"
        class="btn-secondary flex-1 text-xs py-1.5"
        aria-label="截取当前视频画面"
      >
        拍照
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onUnmounted } from 'vue'
import { useWebSocket } from '../composables/useWebSocket'

const { videoFrame, isConnected, videoFps } = useWebSocket()

const videoSrc = ref<string | null>(null)
const fps = computed(() => videoFps.value)

const updateVideo = () => {
  if (!videoFrame.value) {
    return
  }
  videoSrc.value = videoFrame.value
}

const unwatch = watch(videoFrame, updateVideo)

onUnmounted(() => {
  unwatch()
})

const takeSnapshot = () => {
  if (!videoSrc.value) return
  const link = document.createElement('a')
  link.download = `snapshot_${Date.now()}.jpg`
  link.href = videoSrc.value
  document.body.appendChild(link)
  try {
    link.click()
  } catch (error) {
    console.error('[VideoPlayer] 截图下载失败:', error)
  } finally {
    document.body.removeChild(link)
  }
}
</script>