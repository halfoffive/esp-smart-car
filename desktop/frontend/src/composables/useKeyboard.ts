/**
 * 键盘控制组合式函数
 * 管理键盘事件，实现 WASD 控制
 * 
 * 功能：
 * 1. 监听键盘按下/释放事件
 * 2. 映射键盘按键到控制命令
 * 3. 防止重复触发
 * 4. 支持按键组合
 * 
 * 按键映射：
 * - W: 前进
 * - A: 左转
 * - S: 后退
 * - D: 右转
 * - Q: 原地左转
 * - E: 原地右转
 * - 空格: 停止
 * - 1-9: 速度设置
 * - U: 云台上
 * - J: 云台下
 * - H: 云台左
 * - K: 云台右
 * - C: 云台居中
 */

import { ref, onMounted, onUnmounted } from 'vue'

// 当前激活的按键
export const activeKeys = ref<Set<string>>(new Set())

// 当前按下的方向键
let currentDirectionKey: string | null = null

/**
 * 有效的控制键
 */
const VALID_KEYS = new Set([
  'W', 'A', 'S', 'D', 'Q', 'E', ' ',
  'U', 'J', 'H', 'K', 'C',
  '1', '2', '3', '4', '5', '6', '7', '8', '9'
])

/**
 * 方向键映射
 */
const DIRECTION_KEYS = new Set(['W', 'A', 'S', 'D', 'Q', 'E'])

/**
 * 设置键盘事件监听
 * @param sendCommand - 发送命令的回调函数
 */
export const setupKeyboardListeners = (sendCommand: (cmd: string) => void) => {
  /**
   * 处理按键按下
   */
  const handleKeyDown = (event: KeyboardEvent) => {
    const key = event.key.toUpperCase()
    
    // 检查是否为有效的控制键
    if (!VALID_KEYS.has(key)) {
      return
    }
    
    // 阻止默认行为（防止页面滚动等）
    if ([' ', 'ARROWUP', 'ARROWDOWN', 'ARROWLEFT', 'ARROWRIGHT'].includes(key)) {
      event.preventDefault()
    }
    
    // 添加到激活集合
    activeKeys.value.add(key)
    
    // 处理方向键（互斥）
    if (DIRECTION_KEYS.has(key)) {
      // 如果已有方向键按下，先停止
      if (currentDirectionKey && currentDirectionKey !== key) {
        sendCommand(' ')
      }
      currentDirectionKey = key
      sendCommand(key)
    }
    // 处理空格（停止）
    else if (key === ' ') {
      currentDirectionKey = null
      sendCommand(' ')
    }
    // 处理速度键
    else if (key >= '1' && key <= '9') {
      sendCommand(key)
    }
    // 处理云台控制
    else if (['U', 'J', 'H', 'K', 'C'].includes(key)) {
      sendCommand(key)
    }
  }
  
  /**
   * 处理按键释放
   */
  const handleKeyUp = (event: KeyboardEvent) => {
    const key = event.key.toUpperCase()
    
    // 从激活集合移除
    activeKeys.value.delete(key)
    
    // 如果释放的是当前方向键，停止
    if (DIRECTION_KEYS.has(key) && currentDirectionKey === key) {
      currentDirectionKey = null
      sendCommand(' ')
    }
  }
  
  /**
   * 处理窗口失去焦点（自动停止）
   */
  const handleBlur = () => {
    activeKeys.value.clear()
    if (currentDirectionKey) {
      currentDirectionKey = null
      sendCommand(' ')
    }
  }
  
  // 添加事件监听
  window.addEventListener('keydown', handleKeyDown)
  window.addEventListener('keyup', handleKeyUp)
  window.addEventListener('blur', handleBlur)
  
  // 返回清理函数
  return () => {
    window.removeEventListener('keydown', handleKeyDown)
    window.removeEventListener('keyup', handleKeyUp)
    window.removeEventListener('blur', handleBlur)
  }
}

/**
 * 组合式函数
 */
export const useKeyboard = () => {
  let cleanup: (() => void) | null = null
  
  const setup = (sendCommand: (cmd: string) => void) => {
    cleanup = setupKeyboardListeners(sendCommand)
  }
  
  onUnmounted(() => {
    if (cleanup) {
      cleanup()
    }
  })
  
  return {
    activeKeys,
    setupKeyboardListeners: setup
  }
}
