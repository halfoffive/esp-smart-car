/**
 * API 请求组合式函数
 * 封装公共的 fetch + JSON 解析 + 错误处理逻辑
 *
 * 功能：
 * 1. 统一的请求方法（自动设置 Content-Type）
 * 2. 自动 JSON 解析（含空响应保护）
 * 3. 请求超时控制（默认 10 秒）
 * 4. 错误处理（网络错误、HTTP 错误、JSON 解析错误）
 */

import { DEFAULT_API_TOKEN } from '../config/auth'

/** API 响应接口 */
export interface ApiResponse {
  success: boolean
  message?: string
  [key: string]: unknown
}

/** 默认请求超时（毫秒） */
const DEFAULT_TIMEOUT = 10000

/**
 * API 请求组合式函数
 * 提供统一的 HTTP 请求方法，减少重复的 fetch + JSON 代码
 */
export function useApi() {
  /**
   * 发送 HTTP 请求并解析 JSON 响应
   *
   * @param url - 请求 URL
   * @param options - 可选的 RequestInit 配置
   * @returns 解析后的 JSON 响应
   * @throws 网络错误、超时、HTTP 错误或 JSON 解析错误时抛出异常
   */
  const request = async <T = ApiResponse>(url: string, options?: RequestInit & { timeout?: number }): Promise<T> => {
    // 认证：优先使用环境变量，否则使用默认 Token
    const token = (import.meta.env.VITE_API_TOKEN as string | undefined) || DEFAULT_API_TOKEN

    // 默认 headers：认证 + 仅在有请求体时设置 Content-Type
    const defaultHeaders: Record<string, string> = {
      Authorization: `Bearer ${token}`,
    }
    if (options?.body) {
      defaultHeaders['Content-Type'] = 'application/json'
    }

    // 合并 headers：默认 headers 在前，调用方 headers 在后（可覆盖）
    const mergedHeaders = new Headers(defaultHeaders)
    for (const [k, v] of new Headers(options?.headers)) {
      mergedHeaders.set(k, v)
    }

    // 超时控制：默认 10 秒，调用方可通过 options.timeout 覆盖
    const timeout = options?.timeout ?? DEFAULT_TIMEOUT
    const controller = new AbortController()
    const timeoutId = setTimeout(() => controller.abort(), timeout)

    let response: Response
    try {
      response = await fetch(url, {
        ...options,
        headers: mergedHeaders,
        signal: controller.signal,
      })
    } catch (error) {
      clearTimeout(timeoutId)
      if (error instanceof Error && error.name === 'AbortError') {
        throw new Error(`请求超时: ${url}`)
      }
      throw new Error(`网络请求失败: ${error instanceof Error ? error.message : String(error)}`)
    }
    clearTimeout(timeoutId)

    // 检查 HTTP 状态码，避免对非 JSON 响应体调用 json() 导致 SyntaxError
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${response.statusText}`)
    }

    // JSON 解析错误处理：避免后端返回非 JSON 时直接抛出 SyntaxError
    const text = await response.text()
    if (!text.trim()) {
      return {} as T
    }

    try {
      return JSON.parse(text) as T
    } catch (parseError) {
      throw new Error(
        `JSON 解析失败: ${parseError instanceof Error ? parseError.message : String(parseError)}，原始响应: ${text.slice(0, 200)}`
      )
    }
  }

  /**
   * 发送 POST 请求
   *
   * @param url - 请求 URL
   * @param body - 请求体（将被 JSON.stringify）
   * @param timeout - 可选超时（毫秒）
   * @returns 解析后的 JSON 响应
   */
  const post = async <T = ApiResponse>(url: string, body?: unknown, timeout?: number): Promise<T> => {
    return request<T>(url, {
      method: 'POST',
      body: body !== undefined ? JSON.stringify(body) : undefined,
      timeout,
    })
  }

  /**
   * 发送 GET 请求
   *
   * @param url - 请求 URL
   * @param timeout - 可选超时（毫秒）
   * @returns 解析后的 JSON 响应
   */
  const get = async <T = ApiResponse>(url: string, timeout?: number): Promise<T> => {
    return request<T>(url, { timeout })
  }

  return { request, post, get }
}
