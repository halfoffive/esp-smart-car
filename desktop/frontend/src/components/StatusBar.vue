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

    <!-- 链路状态（4 级） -->
    <div
      class="flex items-center gap-1.5 px-1.5 py-0.5 rounded-full border"
      :class="linkStatusStyle.bgClass"
      :title="linkStatusStyle.title"
      aria-label="链路状态"
    >
      <span
        :class="['w-1.5 h-1.5 rounded-full', linkStatusStyle.dotClass]"
        aria-hidden="true"
      ></span>
      <span :class="linkStatusStyle.textClass">{{ linkStatusStyle.text }}</span>
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
      <span class="font-mono">{{ currentSpeedPercent }}%</span>
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

const { isConnected, videoFps, linkStatus } = useWebSocket()
const { status } = useStatus()

const serialConnected = computed(() => status.value.serialStatus.startsWith('已连接'))
const fps = computed(() => videoFps.value || 0)
const currentSpeed = computed(() => status.value.currentSpeed || 0)
const currentSpeedPercent = computed(() => Math.round((currentSpeed.value / 255) * 100))
const frameCount = computed(() => status.value.frameCount || 0)

/** 链路状态等级样式（4 级 + 串口未连接） */
interface LinkStatusStyle {
  text: string
  title: string
  bgClass: string
  textClass: string
  dotClass: string
}

/**
 * 计算 4 级链路状态显示样式：
 * - 串口未连接：灰色
 * - 串口已连接，dongleOk=false：探测中（黄色）
 * - dongleOk=true, carPaired=false：Dongle 已连接（蓝色）
 * - carPaired=true, lastOdomMs > 10000 或 =0（无数据）：车载已配对（黄色）
 * - carPaired=true, lastOdomMs <= 10000：车载在线（绿色）
 */
const linkStatusStyle = computed<LinkStatusStyle>(() => {
  if (!serialConnected.value) {
    return {
      text: '串口未连接',
      title: '串口未连接，请先连接 Dongle',
      bgClass: 'bg-dark-700/50 border-dark-600',
      textClass: 'text-dark-400',
      dotClass: 'bg-dark-500',
    }
  }
  if (!linkStatus.value.dongleOk) {
    return {
      text: '探测中',
      title: '正在探测 Dongle 链路状态',
      bgClass: 'bg-yellow-500/20 border-yellow-500/30',
      textClass: 'text-yellow-400',
      dotClass: 'bg-yellow-400 animate-pulse',
    }
  }
  if (!linkStatus.value.carPaired) {
    return {
      text: 'Dongle 已连接',
      title: 'Dongle 正常，车载 ESP-NOW 未配对',
      bgClass: 'bg-blue-500/20 border-blue-500/30',
      textClass: 'text-blue-400',
      dotClass: 'bg-blue-400',
    }
  }
  // carPaired = true
  // lastOdomMs = 0 表示从未收到车载数据，按"已配对但离线"处理
  if (linkStatus.value.lastOdomMs === 0 || linkStatus.value.lastOdomMs > 10000) {
    return {
      text: '车载已配对',
      title: `车载已配对但离线（${linkStatus.value.lastOdomMs}ms 无数据）`,
      bgClass: 'bg-yellow-500/20 border-yellow-500/30',
      textClass: 'text-yellow-400',
      dotClass: 'bg-yellow-400',
    }
  }
  return {
    text: '车载在线',
    title: `车载在线（${linkStatus.value.lastOdomMs}ms 前收到数据）`,
    bgClass: 'bg-green-500/20 border-green-500/30',
    textClass: 'text-green-400',
    dotClass: 'bg-green-400 animate-pulse',
  }
})
</script>
