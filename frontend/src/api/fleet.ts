import type { HttpClient, QueryParams } from './http'
import type { PageResult } from './system'
import { authSession } from './session.ts'

export interface NodeGroupRow {
  id: number
  parentId: number
  name: string
  sort: number
  remark: string
  nodeCount: number
  createTime?: string
  children?: NodeGroupRow[]
}

export interface NodeGroupWriteRequest {
  parentId?: number
  name?: string
  sort?: number
  remark?: string
}

export interface TelemetryPoint {
  metric: string
  value: number
  unit: string
  collectedAt: string
}

export interface AlarmRow {
  id: number
  ruleId: number
  nodeId: number
  nodeName: string
  level: 'info' | 'warning' | 'critical'
  metric: string
  value: number
  threshold: number
  status: 'open' | 'acknowledged' | 'resolved'
  message: string
  triggeredAt: string
  acknowledgedAt: string
  resolvedAt: string
}

export interface AlarmPage extends PageResult<AlarmRow> {
  openCount: number
}

export interface AlarmRuleRow {
  id: number
  name: string
  nodeId: number
  metric: string
  comparison: string
  threshold: number
  level: 'info' | 'warning' | 'critical'
  enabled: boolean
  cooldownSeconds: number
  createTime: string
}

export interface AlarmRuleWriteRequest {
  name: string
  nodeId: number
  metric: string
  comparison: string
  threshold: number
  level: string
  enabled: boolean
  cooldownSeconds: number
}

export interface CommandRow {
  id: number
  nodeId: number
  nodeName: string
  type: string
  payload: string
  status: 'pending' | 'running' | 'succeeded' | 'failed' | 'timeout' | 'canceled'
  result: string
  message: string
  createdAt: string
  dispatchedAt: string
  finishedAt: string
  timeoutSeconds: number
}

export interface FirmwareRow {
  id: number
  name: string
  version: string
  fileName: string
  contentType: string
  sizeBytes: number
  sha256: string
  createdAt: string
  createdBy: number
}

export interface FirmwareTaskRow {
  id: number
  firmwareId: number
  nodeId: number
  nodeName: string
  status: string
  progress: number
  message: string
  createdAt: string
  updatedAt: string
}

export interface OpsSummary {
  nodes: { total: number; online: number }
  alarms: { open: number }
  telemetry24h: number
  commands: { total: number; pending: number }
}

export function createFleetApi(client: HttpClient) {
  return {
    nodeGroups: {
      tree: () => client.get<NodeGroupRow[]>('/node-groups'),
      create: (body: NodeGroupWriteRequest) => client.post<{ id: number }>('/node-groups', body),
      update: (id: number, body: NodeGroupWriteRequest) => client.put<void>(`/node-groups/${id}`, body),
      remove: (id: number) => client.delete<void>(`/node-groups/${id}`),
    },
    telemetry: {
      latest: (nodeId: number) => client.get<TelemetryPoint[]>('/telemetry/latest', { nodeId }),
      series: (nodeId: number, metric: string, from?: string, to?: string) =>
        client.get<TelemetryPoint[]>('/telemetry/series', { nodeId, metric, from, to }),
    },
    alarms: {
      list: (query?: QueryParams) => client.get<AlarmPage>('/alarms', query),
      ack: (id: number) => client.put<AlarmRow>(`/alarms/${id}/ack`, {}),
      resolve: (id: number) => client.put<AlarmRow>(`/alarms/${id}/resolve`, {}),
      rules: (query?: QueryParams) => client.get<PageResult<AlarmRuleRow>>('/alarm-rules', query),
      createRule: (body: AlarmRuleWriteRequest) => client.post<{ id: number }>('/alarm-rules', body),
      updateRule: (id: number, body: Partial<AlarmRuleWriteRequest>) => client.put<void>(`/alarm-rules/${id}`, body),
      removeRule: (id: number) => client.delete<void>(`/alarm-rules/${id}`),
    },
    commands: {
      list: (query?: QueryParams) => client.get<PageResult<CommandRow>>('/commands', query),
      create: (body: { nodeId: number; type: string; payload: string; timeoutSeconds: number }) => client.post<CommandRow>('/commands', body),
      cancel: (id: number) => client.put<CommandRow>(`/commands/${id}/cancel`, {}),
    },
    firmware: {
      list: (query?: QueryParams) => client.get<PageResult<FirmwareRow>>('/firmware', query),
      remove: (id: number) => client.delete<void>(`/firmware/${id}`),
      tasks: (id: number) => client.get<FirmwareTaskRow[]>(`/firmware/${id}/tasks`),
      deploy: (id: number, nodeIds: number[]) => client.post<Array<{ nodeId: number; taskId: number; commandId: number }>>(`/firmware/${id}/deploy`, { nodeIds }),
      upload: (file: File, name: string, version: string, onProgress?: (percent: number) => void) => uploadFirmware(file, name, version, onProgress),
    },
    ops: {
      summary: () => client.get<OpsSummary>('/ops/summary'),
      protocols: () => client.get<Record<string, unknown>>('/ops/protocols'),
    },
  }
}

function uploadFirmware(file: File, name: string, version: string, onProgress?: (percent: number) => void): Promise<FirmwareRow> {
  return new Promise((resolve, reject) => {
    const token = authSession.token()
    if (!token) {
      reject(new Error('登录已过期，请重新登录'))
      return
    }
    const query = new URLSearchParams({ name, version, fileName: file.name })
    const request = new XMLHttpRequest()
    request.open('POST', `/api/firmware?${query.toString()}`)
    request.setRequestHeader('Authorization', `Bearer ${token}`)
    request.setRequestHeader('Content-Type', file.type || 'application/octet-stream')
    request.upload.onprogress = event => {
      if (event.lengthComputable && onProgress) onProgress(Math.round((event.loaded / event.total) * 100))
    }
    request.onerror = () => reject(new Error('固件上传失败：网络不可用'))
    request.onload = () => {
      try {
        const body = JSON.parse(request.responseText) as { code: number; message: string; data: FirmwareRow }
        if (request.status >= 200 && request.status < 300 && body.code === 200) resolve(body.data)
        else reject(new Error(body.message || `上传失败（HTTP ${request.status}）`))
      } catch {
        reject(new Error(`上传失败（HTTP ${request.status}）`))
      }
    }
    request.send(file)
  })
}
