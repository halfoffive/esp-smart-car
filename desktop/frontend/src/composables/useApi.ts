/**
 * API 请求组合式函数
 * 封装公共的 fetch + JSON 解析 + 错误处理逻辑
 * 
 * 功能：
 * 1. 统一的请求方法（自动设置 Content-Type）
 * 2. 自动 JSON 解析
 * 3. 错误处理（网络错误、HTTP 错误）
 */

/** API 响应接口 */
export interface ApiResponse {
  success: boolean
  message?: string
  [key: string]: unknown
}

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
   * @throws 网络错误或 JSON 解析错误时抛出异常
   */
  const request = async <T = ApiResponse>(url: string, options?: RequestInit): Promise<T> => {
    // 默认 headers：仅在有请求体时设置 Content-Type
    const defaultHeaders: Record<string, string> = {}
    if (options?.body) {
      defaultHeaders['Content-Type'] = 'application/json'
    }

    // 深度合并 headers：默认 headers 在前，调用方 headers 在后（可覆盖）
    const mergedHeaders: Record<string, string> = {
      ...defaultHeaders,
      ...(options?.headers as Record<string, string> | undefined),
    }

    const response = await fetch(url, {
      ...options,
      headers: mergedHeaders,
    })

    // 检查 HTTP 状态码，避免对非 JSON 响应体调用 json() 导致 SyntaxError
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}: ${response.statusText}`)
    }

    return response.json() as Promise<T>
  }

  /**
   * 发送 POST 请求
   * 
   * @param url - 请求 URL
   * @param body - 请求体（将被 JSON.stringify）
   * @returns 解析后的 JSON 响应
   */
  const post = async <T = ApiResponse>(url: string, body?: unknown): Promise<T> => {
    return request<T>(url, {
      method: 'POST',
      body: body ? JSON.stringify(body) : undefined,
    })
  }

  /**
   * 发送 GET 请求
   * 
   * @param url - 请求 URL
   * @returns 解析后的 JSON 响应
   */
  const get = async <T = ApiResponse>(url: string): Promise<T> => {
    return request<T>(url)
  }

  return { request, post, get }
}
