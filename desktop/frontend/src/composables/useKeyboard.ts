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

import { reactive, ref, onMounted, onUnmounted } from 'vue'
import { COMMAND_REPEAT_INTERVAL_MS } from '../config/constants'

/** 有效的控制键集合 */
const VALID_KEYS = new Set([
  'W', 'A', 'S', 'D', 'Q', 'E', ' ',
  '1', '2', '3', '4', '5', '6', '7', '8', '9'
])

/** 方向键集合（互斥控制） */
const DIRECTION_KEYS = new Set(['W', 'A', 'S', 'D', 'Q', 'E'])

/** 需要阻止默认行为的按键集合 */
const PREVENT_DEFAULT_KEYS = new Set([' '])

/**
 * 键盘控制组合式函数
 *
 * 标准 composable 风格：内部自动使用 onMounted/onUnmounted 管理事件监听器生命周期
 * 调用者只需 `useKeyboard(sendCommand, setSpeed)` 即可，无需手动清理
 *
 * @param sendCommand - 发送命令的回调函数
 * @param setSpeed - 设置速度 PWM 的回调函数（接收 0-255 数值）
 */
export function useKeyboard(sendCommand: (cmd: string) => void, setSpeed: (pwm: number) => void) {
  const activeKeys = reactive(new Set<string>())
  const currentDirectionKey = ref<string | null>(null)

  let commandRepeatTimer: ReturnType<typeof setInterval> | null = null

  const stopCommandRepeat = () => {
    if (commandRepeatTimer) {
      clearInterval(commandRepeatTimer)
      commandRepeatTimer = null
    }
  }

  const startCommandRepeat = (command: string) => {
    stopCommandRepeat()
    commandRepeatTimer = setInterval(() => {
      sendCommand(command)
    }, COMMAND_REPEAT_INTERVAL_MS)
  }

  const pauseKeyboard = () => {
    stopCommandRepeat()
  }

  const resumeKeyboard = () => {
    if (currentDirectionKey.value) {
      sendCommand(currentDirectionKey.value)
      startCommandRepeat(currentDirectionKey.value)
    }
  }

  const isInputFocused = () => {
    const activeEl = document.activeElement
    return activeEl && (activeEl.tagName === 'INPUT' || activeEl.tagName === 'TEXTAREA' || activeEl.tagName === 'SELECT' || (activeEl as HTMLElement).isContentEditable)
  }

  const handleKeyDown = (event: KeyboardEvent) => {
    if (event.repeat) return;
    if (event.isComposing) return;
    if (isInputFocused()) return;

    if (PREVENT_DEFAULT_KEYS.has(event.key)) {
      event.preventDefault()
    }

    const key = event.key.toUpperCase()

    if (!VALID_KEYS.has(key)) {
      return
    }

    activeKeys.add(key)

    if (DIRECTION_KEYS.has(key)) {
      if (currentDirectionKey.value && currentDirectionKey.value !== key) {
        stopCommandRepeat()
        sendCommand(' ')
      }
      currentDirectionKey.value = key
      sendCommand(key)
      startCommandRepeat(key)
    }
    else if (key === ' ') {
      currentDirectionKey.value = null
      stopCommandRepeat()
      sendCommand(' ')
    }
    else if (key >= '1' && key <= '9') {
      const pwm = Math.round((parseInt(key, 10) - 1) / 8 * 227 + 28)
      setSpeed(pwm)
    }
  }

  const handleKeyUp = (event: KeyboardEvent) => {
    if (event.isComposing) return;
    if (isInputFocused()) return;

    const key = event.key.toUpperCase()

    activeKeys.delete(key)

    if (DIRECTION_KEYS.has(key) && currentDirectionKey.value === key) {
      currentDirectionKey.value = null
      stopCommandRepeat()
      sendCommand(' ')
    }
  }

  const handleBlur = () => {
    activeKeys.clear()
    if (currentDirectionKey.value) {
      currentDirectionKey.value = null
      stopCommandRepeat()
      sendCommand(' ')
    }
  }

  const handleVisibilityChange = () => {
    if (document.hidden) {
      activeKeys.clear()
      if (currentDirectionKey.value) {
        currentDirectionKey.value = null
        stopCommandRepeat()
        sendCommand(' ')
      }
    }
  }

  onMounted(() => {
    window.addEventListener('keydown', handleKeyDown)
    window.addEventListener('keyup', handleKeyUp)
    window.addEventListener('blur', handleBlur)
    document.addEventListener('visibilitychange', handleVisibilityChange)
  })

  onUnmounted(() => {
    stopCommandRepeat()
    window.removeEventListener('keydown', handleKeyDown)
    window.removeEventListener('keyup', handleKeyUp)
    window.removeEventListener('blur', handleBlur)
    document.removeEventListener('visibilitychange', handleVisibilityChange)
  })

  return {
    activeKeys,
    currentDirectionKey,
    pauseKeyboard,
    resumeKeyboard,
  }
}
