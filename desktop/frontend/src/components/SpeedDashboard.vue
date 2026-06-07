<template>
  <div class="grid grid-cols-2 gap-2">
    <!-- 当前速度 -->
    <div class="speed-module">
      <div class="flex items-center gap-1 mb-0.5">
        <svg class="w-3 h-3 text-primary-400" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z"/>
        </svg>
        <span class="text-[10px] text-dark-400 font-medium">当前速度</span>
      </div>
      <div class="flex items-baseline gap-1">
        <span class="text-xl font-bold font-mono text-primary-400">{{ displayCurrentSpeed }}</span>
        <span class="text-[10px] text-dark-500">%</span>
      </div>
      <div class="speed-bar mt-1">
        <div class="speed-fill bg-primary-500" :style="{ width: currentSpeedPercent + '%' }"></div>
      </div>
    </div>

    <!-- 最高速度 -->
    <div class="speed-module">
      <div class="flex items-center gap-1 mb-0.5">
        <svg class="w-3 h-3 text-red-400" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5.293 9.293a1 1 0 011.414 0l5 5a1 1 0 01-1.414 1.414l-5-5a1 1 0 010-1.414zM14 7.586l-2 2V4h2v3.586z"/>
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 20a8 8 0 100-16 8 8 0 000 16z"/>
        </svg>
        <span class="text-[10px] text-dark-400 font-medium">最高速度</span>
      </div>
      <div class="flex items-baseline gap-1">
        <span class="text-xl font-bold font-mono text-red-400">{{ displayMaxSpeed }}</span>
        <span class="text-[10px] text-dark-500">%</span>
      </div>
      <div class="speed-bar mt-1">
        <div class="speed-fill bg-red-500" :style="{ width: maxSpeedPercent + '%' }"></div>
      </div>
    </div>

    <!-- 平均速度 -->
    <div class="speed-module">
      <div class="flex items-center gap-1 mb-0.5">
        <svg class="w-3 h-3 text-green-400" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 19v-6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2a2 2 0 002-2zm0 0V9a2 2 0 012-2h2a2 2 0 012 2v10m-6 0a2 2 0 002 2h2a2 2 0 002-2m0 0V5a2 2 0 012-2h2a2 2 0 012 2v14a2 2 0 01-2 2h-2a2 2 0 01-2-2z"/>
        </svg>
        <span class="text-[10px] text-dark-400 font-medium">平均速度</span>
      </div>
      <div class="flex items-baseline gap-1">
        <span class="text-xl font-bold font-mono text-green-400">{{ displayAvgSpeed }}</span>
        <span class="text-[10px] text-dark-500">%</span>
      </div>
      <div class="speed-bar mt-1">
        <div class="speed-fill bg-green-500" :style="{ width: avgSpeedPercent + '%' }"></div>
      </div>
    </div>

    <!-- 速度记录 -->
    <div class="speed-module">
      <div class="flex items-center gap-1 mb-0.5">
        <svg class="w-3 h-3 text-yellow-400" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 8v4l3 3m6-3a9 9 0 11-18 0 9 9 0 0118 0z"/>
        </svg>
        <span class="text-[10px] text-dark-400 font-medium">运行时长</span>
      </div>
      <div class="flex items-baseline gap-1">
        <span class="text-xl font-bold font-mono text-yellow-400">{{ displayRunTime }}</span>
        <span class="text-[10px] text-dark-500">{{ runTimeUnit }}</span>
      </div>
      <div class="flex items-center gap-1 mt-1.5">
        <span class="text-[10px] text-dark-500">命令数</span>
        <span class="text-xs font-mono text-dark-300">{{ commandCount }}</span>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'

const currentSpeedLevel = ref(5)
const maxSpeedLevel = ref(5)
const avgSpeedLevel = ref(5)
const commandCount = ref(0)
const runStartTime = ref<number>(Date.now())
const runTimeSeconds = ref(0)

// 速度历史记录（用于计算平均值）
const speedHistory = ref<number[]>([])
const MAX_HISTORY = 100

const speedToPercent = (level: number) => Math.round((level / 9) * 100)

const currentSpeedPercent = computed(() => speedToPercent(currentSpeedLevel.value))
const maxSpeedPercent = computed(() => speedToPercent(maxSpeedLevel.value))
const avgSpeedPercent = computed(() => speedToPercent(avgSpeedLevel.value))

const displayCurrentSpeed = computed(() => currentSpeedPercent.value)
const displayMaxSpeed = computed(() => maxSpeedPercent.value)
const displayAvgSpeed = computed(() => avgSpeedPercent.value)

const displayRunTime = computed(() => {
  const s = runTimeSeconds.value
  if (s < 60) return `${s}`
  if (s < 3600) return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`
  return `${Math.floor(s / 3600)}:${String(Math.floor((s % 3600) / 60)).padStart(2, '0')}`
})

const runTimeUnit = computed(() => {
  const s = runTimeSeconds.value
  if (s < 60) return '秒'
  if (s < 3600) return '分钟'
  return '小时'
})

// 定期更新状态
let statusInterval: number
let timeInterval: number

const updateStatus = async () => {
  try {
    const response = await fetch('/api/status')
    const status = await response.json()
    
    const newSpeed = status.current_speed || 5
    
    // 更新当前速度
    currentSpeedLevel.value = newSpeed
    
    // 更新最高速度
    if (newSpeed > maxSpeedLevel.value) {
      maxSpeedLevel.value = newSpeed
    }
    
    // 记录速度历史
    speedHistory.value.push(newSpeed)
    if (speedHistory.value.length > MAX_HISTORY) {
      speedHistory.value.shift()
    }
    
    // 计算平均速度
    const sum = speedHistory.value.reduce((a, b) => a + b, 0)
    avgSpeedLevel.value = Math.round(sum / speedHistory.value.length)
    
    // 更新命令数
    if (status.command_count !== undefined) {
      commandCount.value = status.command_count
    }
  } catch {
    // 忽略错误
  }
}

const updateRunTime = () => {
  runTimeSeconds.value = Math.floor((Date.now() - runStartTime.value) / 1000)
}

const resetMaxSpeed = () => {
  maxSpeedLevel.value = currentSpeedLevel.value
  speedHistory.value = []
}

onMounted(() => {
  runStartTime.value = Date.now()
  updateStatus()
  statusInterval = setInterval(updateStatus, 500) as unknown as number
  timeInterval = setInterval(updateRunTime, 1000) as unknown as number
})

onUnmounted(() => {
  clearInterval(statusInterval)
  clearInterval(timeInterval)
})

defineExpose({ resetMaxSpeed })
</script>

<style scoped>
.speed-module {
  @apply bg-dark-800 rounded-lg p-2.5;
}

.speed-bar {
  @apply h-1 bg-dark-700 rounded-full overflow-hidden;
}

.speed-fill {
  @apply h-full rounded-full transition-all duration-300;
}
</style>