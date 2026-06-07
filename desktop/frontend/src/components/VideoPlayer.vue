<template>
  <div class="panel flex-1 flex flex-col min-h-0">
    <div class="panel-header flex items-center justify-between py-2">
      <span class="text-sm">实时视频</span>
      <div class="flex items-center gap-2">
        <span v-if="fps > 0" class="text-xs text-primary-400 font-mono">
          {{ fps }} FPS
        </span>
        <span v-if="resolution" class="text-[10px] text-dark-500">
          {{ resolution }}
        </span>
      </div>
    </div>
    
    <div class="video-container flex-1 min-h-0 relative">
      <img 
        v-if="videoSrc"
        :src="videoSrc"
        alt="实时视频"
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
      
      <div v-if="isRecording" class="absolute top-2 left-2 flex items-center gap-1 bg-red-600/90 text-white px-2 py-0.5 rounded-full text-xs">
        <span class="w-1.5 h-1.5 bg-white rounded-full animate-pulse"></span>
        录制中
      </div>
      
      <div class="absolute top-2 right-2">
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
        @click="toggleRecording"
        :class="isRecording ? 'btn-danger' : 'btn-secondary'"
        class="flex-1 text-xs py-1.5"
      >
        <span v-if="isRecording">停止录制</span>
        <span v-else>录制</span>
      </button>
      
      <button 
        @click="takeSnapshot"
        class="btn-secondary flex-1 text-xs py-1.5"
      >
        拍照
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { useWebSocket } from '../composables/useWebSocket'

const { videoFrame, isConnected } = useWebSocket()

const videoSrc = ref<string | null>(null)
const fps = ref(0)
const resolution = ref('')
const isRecording = ref(false)

let frameCount = 0
let lastFpsUpdate = 0

const updateVideo = () => {
  if (videoFrame.value) {
    videoSrc.value = videoFrame.value
    
    const now = Date.now()
    frameCount++
    
    if (now - lastFpsUpdate >= 1000) {
      fps.value = frameCount
      frameCount = 0
      lastFpsUpdate = now
    }
  }
  
  requestAnimationFrame(updateVideo)
}

onMounted(() => {
  requestAnimationFrame(updateVideo)
})

const toggleRecording = () => {
  isRecording.value = !isRecording.value
}

const takeSnapshot = () => {
  if (videoSrc.value) {
    const link = document.createElement('a')
    link.download = `snapshot_${Date.now()}.jpg`
    link.href = videoSrc.value
    link.click()
  }
}
</script>