import type { HttpClient, QueryParams } from './http'

export interface DashboardActivity {
  who: string
  target: string
  time: string
  tint: string
}

export interface DashboardSummary {
  stats: Array<Record<string, unknown>>
  trend: number[]
  activities: DashboardActivity[]
  roleDist: Array<{ label: string; value: number }>
  trendTotal: number
  successRate: number
  /** 上一等长周期的登录次数，用于环比 */
  trendPrev: number
  /** 近 7 天按日操作量：date 日期、total 操作数、failed 失败数 */
  dailyOps: Array<{ date: string; total: number; failed: number }>
}

/** 服务健康：status 运行状态（UP 正常）、time 服务端时间 */
export interface HealthStatus {
  status: string
  time: string
}

export function createPortalApi(client: HttpClient) {
  return {
    health: () => client.get<HealthStatus>('/health'),
    dashboard: {
      summary: (query?: QueryParams) => client.get<DashboardSummary>('/dashboard/summary', query),
    },
  }
}
