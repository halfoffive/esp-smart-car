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

    <!-- 轮子转速（RPM） -->
    <div class="speed-module" aria-label="轮子转速">
      <div class="flex items-center gap-1 mb-0.5">
        <svg class="w-3 h-3 text-cyan-400" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 8v4l3 3m6-3a9 9 0 11-18 0 9 9 0 0118 0z"/>
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 2a10 10 0 100 20 10 10 0 000-20z"/>
        </svg>
        <span class="text-[10px] text-dark-400 font-medium">轮子转速</span>
      </div>
      <div class="flex items-baseline gap-1.5">
        <span class="text-sm font-bold font-mono text-cyan-400">L {{ leftRpm }}</span>
        <span class="text-sm font-bold font-mono text-cyan-400">R {{ rightRpm }}</span>
        <span class="text-[10px] text-dark-500">RPM</span>
      </div>
      <div class="speed-bar mt-1">
        <div class="speed-fill bg-cyan-500" :style="{ width: wheelRpmBarPercent + '%' }"></div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useWebSocket } from '../composables/useWebSocket'

// WebSocket 测速数据
const { odometry } = useWebSocket()

const MAX_SPEED_MMS = 500
const WHEEL_DIAMETER_MM = 65

const leftSpeedCm = computed(() => String((odometry.value.leftSpeed / 10).toFixed(1)))
const rightSpeedCm = computed(() => String((odometry.value.rightSpeed / 10).toFixed(1)))

const currentSpeedBarPercent = computed(() => {
  const maxWheelSpeed = Math.max(Math.abs(odometry.value.leftSpeed), Math.abs(odometry.value.rightSpeed))
  return Math.min(100, Math.round((maxWheelSpeed / MAX_SPEED_MMS) * 100))
})

const mmpsToRpm = (mmps: number): number => {
  const circumference = Math.PI * WHEEL_DIAMETER_MM
  return (mmps * 60) / circumference
}

const MAX_RPM = mmpsToRpm(MAX_SPEED_MMS)

const leftRpm = computed(() => String(mmpsToRpm(odometry.value.leftSpeed).toFixed(0)))
const rightRpm = computed(() => String(mmpsToRpm(odometry.value.rightSpeed).toFixed(0)))

// 轮子转速条百分比（取左右轮绝对值较大者）
const wheelRpmBarPercent = computed(() => {
  const maxRpm = Math.max(
    Math.abs(mmpsToRpm(odometry.value.leftSpeed)),
    Math.abs(mmpsToRpm(odometry.value.rightSpeed))
  )
  return Math.min(100, Math.round((maxRpm / MAX_RPM) * 100))
})
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
  transition: width 0.3s ease;
}
</style>
