<template>
  <div class="grid grid-cols-2 gap-2" role="region" aria-label="速度仪表盘">
    <!-- 当前速度（左右轮实际速度） -->
    <div class="speed-module" aria-label="当前车轮速度">
      <div class="flex items-center gap-1 mb-0.5">
        <svg class="w-3 h-3 text-primary-400" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z"/>
        </svg>
        <span class="text-[10px] text-dark-400 font-medium">当前速度</span>
      </div>
      <div class="flex items-baseline gap-1.5">
        <span class="text-sm font-bold font-mono text-primary-400">L {{ leftSpeedCm }}</span>
        <span class="text-sm font-bold font-mono text-primary-400">R {{ rightSpeedCm }}</span>
        <span class="text-[10px] text-dark-500">cm/s</span>
      </div>
      <div class="speed-bar mt-1">
        <div class="speed-fill bg-primary-500" :style="{ width: currentSpeedBarPercent + '%' }"></div>
      </div>
    </div>

    <!-- 最高速度 -->
    <div class="speed-module" aria-label="最高速度">
      <div class="flex items-center gap-1 mb-0.5">
        <svg class="w-3 h-3 text-red-400" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5.293 9.293a1 1 0 011.414 0l5 5a1 1 0 01-1.414 1.414l-5-5a1 1 0 010-1.414zM14 7.586l-2 2V4h2v3.586z"/>
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 20a8 8 0 100-16 8 8 0 000 16z"/>
        </svg>
        <span class="text-[10px] text-dark-400 font-medium">最高速度</span>
      </div>
      <div class="flex items-baseline gap-1">
        <span class="text-xl font-bold font-mono text-red-400">{{ maxSpeedCm }}</span>
        <span class="text-[10px] text-dark-500">cm/s</span>
      </div>
      <div class="speed-bar mt-1">
        <div class="speed-fill bg-red-500" :style="{ width: maxSpeedBarPercent + '%' }"></div>
      </div>
    </div>

    <!-- 平均速度 -->
    <div class="speed-module" aria-label="平均速度">
      <div class="flex items-center gap-1 mb-0.5">
        <svg class="w-3 h-3 text-green-400" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 19v-6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2a2 2 0 002-2zm0 0V9a2 2 0 012-2h2a2 2 0 012 2v10m-6 0a2 2 0 002 2h2a2 2 0 002-2m0 0V5a2 2 0 012-2h2a2 2 0 012 2v14a2 2 0 01-2 2h-2a2 2 0 01-2-2z"/>
        </svg>
        <span class="text-[10px] text-dark-400 font-medium">平均速度</span>
      </div>
      <div class="flex items-baseline gap-1">
        <span class="text-xl font-bold font-mono text-green-400">{{ avgSpeedCm }}</span>
        <span class="text-[10px] text-dark-500">cm/s</span>
      </div>
      <div class="speed-bar mt-1">
        <div class="speed-fill bg-green-500" :style="{ width: avgSpeedBarPercent + '%' }"></div>
      </div>
    </div>

    <!-- 运行时长 -->
    <div class="speed-module" aria-label="运行时长与命令数">
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
import { ref, computed, watch } from 'vue'
import { useWebSocket } from '../composables/useWebSocket'
import { useStatus } from '../composables/useStatus'

// WebSocket 测速数据
const { odometry } = useWebSocket()
// 共享状态轮询（命令数）
const { status } = useStatus()

// 速度条最大参考值：50 cm/s（对应 500 mm/s）
const MAX_SPEED_MMS = 500

// 当前左右轮速度（cm/s）
const leftSpeedCm = computed(() => (odometry.value.leftSpeed / 10).toFixed(1))
const rightSpeedCm = computed(() => (odometry.value.rightSpeed / 10).toFixed(1))

// 当前速度条百分比（取左右轮绝对值较大者）
const currentSpeedBarPercent = computed(() => {
  const maxWheelSpeed = Math.max(Math.abs(odometry.value.leftSpeed), Math.abs(odometry.value.rightSpeed))
  return Math.min(100, Math.round((maxWheelSpeed / MAX_SPEED_MMS) * 100))
})

// 最高速度追踪（mm/s）
const maxSpeedMms = ref(0)
const maxSpeedCm = computed(() => (maxSpeedMms.value / 10).toFixed(1))
const maxSpeedBarPercent = computed(() => Math.min(100, Math.round((maxSpeedMms.value / MAX_SPEED_MMS) * 100)))

// 平均速度追踪
const speedSamples = ref<number[]>([])
const MAX_SAMPLES = 100
const avgSpeedMms = ref(0)
const avgSpeedCm = computed(() => (avgSpeedMms.value / 10).toFixed(1))
const avgSpeedBarPercent = computed(() => Math.min(100, Math.round((avgSpeedMms.value / MAX_SPEED_MMS) * 100)))

// 平均速度增量计算（避免每次 reduce 遍历整个数组）
const runningSum = ref(0)

// 监听测速数据更新，追踪最高速度和平均速度
watch(odometry, (newOdom) => {
  const leftAbs = Math.abs(newOdom.leftSpeed)
  const rightAbs = Math.abs(newOdom.rightSpeed)
  const currentMax = Math.max(leftAbs, rightAbs)

  // 更新最高速度
  if (currentMax > maxSpeedMms.value) {
    maxSpeedMms.value = currentMax
  }

  // 记录速度样本（取两轮绝对值平均值）
  const avg = (leftAbs + rightAbs) / 2
  speedSamples.value.push(avg)
  runningSum.value += avg

  // 超过最大样本数时截断（比 shift() O(n) 更高效）
  if (speedSamples.value.length > MAX_SAMPLES) {
    // 移除最旧的样本，从 runningSum 中减去
    const removed = speedSamples.value.slice(0, speedSamples.value.length - MAX_SAMPLES)
    for (const v of removed) {
      runningSum.value -= v
    }
    speedSamples.value = speedSamples.value.slice(-MAX_SAMPLES)
  }

  // 计算平均速度（增量求和，无需 reduce）
  avgSpeedMms.value = runningSum.value / speedSamples.value.length
})

// 运行时长（基于后端 uptime）
const commandCount = computed(() => status.value.command_count || 0)
const runTimeSeconds = computed(() => status.value.uptime || 0)

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

const resetMaxSpeed = () => {
  maxSpeedMms.value = 0
  speedSamples.value = []
  runningSum.value = 0
  avgSpeedMms.value = 0
}

defineExpose({ resetMaxSpeed })
</script>

<style scoped>
.speed-module {
  background-color: var(--color-dark-800);
  border-radius: 0.5rem;
  padding: 0.625rem;
}

.speed-bar {
  height: 0.25rem;
  background-color: var(--color-dark-700);
  border-radius: 9999px;
  overflow: hidden;
}

.speed-fill {
  height: 100%;
  border-radius: 9999px;
  transition: all 0.3s;
}
</style>
