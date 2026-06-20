/**
 * 键盘控制组合式函数
 * 管理键盘事件，实现 WASD 控制
 * 
 * 功能：
 * 1. 监听键盘按下/释放事件
 * 2. 映射键盘按键到控制命令
 * 3. 防止重复触发
 * 4. 支持按键组合
 * 5. 自动生命周期管理（onMounted/onUnmounted）
 * 
 * 按键映射：
 * - W: 前进
 * - A: 左转
 * - S: 后退
 * - D: 右转
 * - Q: 原地左转
 * - E: 原地右转
 * - 空格: 停止
 * - 1-9: 速度快捷档位（映射到 0-255 PWM）
 */

import { ref, onMounted, onUnmounted } from 'vue'

/** 有效的控制键集合 */
const VALID_KEYS = new Set([
  'W', 'A', 'S', 'D', 'Q', 'E', ' ',
  '1', '2', '3', '4', '5', '6', '7', '8', '9'
])

/** 方向键集合（互斥控制） */
const DIRECTION_KEYS = new Set(['W', 'A', 'S', 'D', 'Q', 'E'])

/** 需要阻止默认行为的按键集合 */
const PREVENT_DEFAULT_KEYS = new Set(['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', ' '])

/**
 * 键盘控制组合式函数
 *
 * 标准 composable 风格：内部自动使用 onMounted/onUnmounted 管理事件监听器生命周期
 * 调用者只需 `useKeyboard(sendCommand, setSpeed)` 即可，无需手动清理
 *
 * @param sendCommand - 发送命令的回调函数
 * @param setSpeed - 设置速度 PWM 的回调函数（接收 0-255 数值）
 */
export const useKeyboard = (sendCommand: (cmd: string) => void, setSpeed: (pwm: number) => void) => {
  // 当前激活的按键（响应式，供 UI 高亮显示）
  const activeKeys = ref<Set<string>>(new Set())

  // 当前按下的方向键（闭包内部状态，确保互斥）
  let currentDirectionKey: string | null = null

  /** 处理按键按下 */
  const handleKeyDown = (event: KeyboardEvent) => {
    // 忽略 OS 按键重复事件，防止命令风暴
    if (event.repeat) return;

    // 忽略 IME 输入过程中的按键（如中文拼音输入等组合输入）
    if (event.isComposing) return;

    // 忽略输入框/文本域/下拉框中的按键，防止在输入 MAC 地址或选择串口时误触发车辆控制
    const activeEl = document.activeElement
    if (activeEl && (activeEl.tagName === 'INPUT' || activeEl.tagName === 'TEXTAREA' || activeEl.tagName === 'SELECT' || (activeEl as HTMLElement).isContentEditable)) {
      return
    }

    // 阻止箭头键和空格的默认行为（防止页面滚动等）
    // 注意：event.key 对箭头键始终为首字母大写格式（如 'ArrowUp'），需在 toUpperCase 之前检查
    if (PREVENT_DEFAULT_KEYS.has(event.key)) {
      event.preventDefault()
    }

    const key = event.key.toUpperCase()

    // 检查是否为有效的控制键
    if (!VALID_KEYS.has(key)) {
      return
    }

    // 添加到激活集合（替换整个 Set 以触发 Vue 响应式）
    activeKeys.value = new Set(activeKeys.value).add(key)

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
    // 处理速度键：将 1-9 映射为近似 PWM 值（1→28, 9→255）
    else if (key >= '1' && key <= '9') {
      const pwm = Math.round((parseInt(key, 10) - 1) / 8 * 227 + 28)
      setSpeed(pwm)
    }
  }

  /** 处理按键释放 */
  const handleKeyUp = (event: KeyboardEvent) => {
    const key = event.key.toUpperCase()

    // 从激活集合移除（替换整个 Set 以触发 Vue 响应式）
    activeKeys.value = new Set([...activeKeys.value].filter(k => k !== key))

    // 如果释放的是当前方向键，停止
    if (DIRECTION_KEYS.has(key) && currentDirectionKey === key) {
      currentDirectionKey = null
      sendCommand(' ')
    }
  }

  /** 处理窗口失去焦点（自动停止所有运动） */
  const handleBlur = () => {
    activeKeys.value = new Set()
    if (currentDirectionKey) {
      currentDirectionKey = null
      sendCommand(' ')
    }
  }

  /** 处理页面可见性变化（切换标签页/最小化时自动停止） */
  const handleVisibilityChange = () => {
    if (document.hidden) {
      activeKeys.value = new Set()
      if (currentDirectionKey) {
        currentDirectionKey = null
        sendCommand(' ')
      }
    }
  }

  // 自动生命周期管理：组件挂载时添加监听器，卸载时自动清理
  onMounted(() => {
    window.addEventListener('keydown', handleKeyDown)
    window.addEventListener('keyup', handleKeyUp)
    window.addEventListener('blur', handleBlur)
    document.addEventListener('visibilitychange', handleVisibilityChange)
  })

  onUnmounted(() => {
    window.removeEventListener('keydown', handleKeyDown)
    window.removeEventListener('keyup', handleKeyUp)
    window.removeEventListener('blur', handleBlur)
    document.removeEventListener('visibilitychange', handleVisibilityChange)
  })

  return {
    activeKeys
  }
}
