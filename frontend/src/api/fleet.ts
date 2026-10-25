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
  scheduledAt: string
  batchId: number
}

export interface FirmwareRow {
  id: number
  name: string
  version: string
  fileName: string
  contentType: string
  sizeBytes: number
  sha256: string
  signed: boolean
  signatureKeyId: string
  signatureAlgorithm: string
  createdAt: string
  createdBy: number
}

export interface FirmwarePolicy {
  requireSignature: boolean
  trustedKeyIds: string[]
}

export interface FirmwareUploadOptions {
  signature?: string
  signingKeyId?: string
  onProgress?: (percent: number) => void
  tenantId?: number
}

export interface FirmwareDeployItem {
  nodeId: number
  taskId: number
  commandId: number
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

export type FirmwareRolloutStatus =
  | 'draft'
  | 'scheduled'
  | 'preparing'
  | 'running'
  | 'observing'
  | 'paused'
  | 'rolling_back'
  | 'rollback_observing'
  | 'rolled_back'
  | 'completed'
  | 'aborted'
  | 'failed'
  | 'rollback_unavailable'

export type FirmwareRolloutWaveStatus =
  | 'pending'
  | 'dispatching'
  | 'observing'
  | 'passed'
  | 'failed'
  | 'skipped'
  | 'rolling_back'
  | 'rolled_back'

export type FirmwareRolloutTargetStatus =
  | 'pending'
  | 'dispatching'
  | 'dispatched'
  | 'downloading'
  | 'installing'
  | 'installed'
  | 'healthy'
  | 'failed'
  | 'unreachable'
  | 'rolling_back'
  | 'rolled_back'
  | 'rollback_failed'
  | 'rollback_unavailable'

/** 波次规模：percent 与 size 二选一；percent 表示累计覆盖比例 */
export interface FirmwareRolloutWaveSpec {
  percent?: number
  size?: number
}

export interface FirmwareRolloutMaintenanceWindow {
  start: string
  end: string
  days?: number[]
}

export interface FirmwareRolloutPolicy {
  waves: FirmwareRolloutWaveSpec[]
  maxConcurrentPerWave: number
  settleSeconds: number
  observeSeconds: number
  maxWaveSeconds: number
  minWaveSuccessRatio: number
  minCumulativeSuccessRatio: number
  maxFailures: number
  autoRollback: boolean
  onTimeout: 'pause' | 'fail'
  requiredFreshTelemetrySeconds: number
  exemptAlarmMetrics: string[]
  maintenanceWindow?: FirmwareRolloutMaintenanceWindow | null
  requireInstalledVersion: boolean
}

export interface FirmwareRolloutGateEntry {
  reason?: string
  passed?: boolean
  forced?: boolean
  timeout?: boolean
  healthy?: number
  failed?: number
  total?: number
  at?: string
  [key: string]: unknown
}

export interface FirmwareRolloutWave {
  id: number
  rolloutId: number
  waveIndex: number
  status: FirmwareRolloutWaveStatus
  targetCount: number
  dispatchedAt: string
  deadlineAt: string
  settledAt: string
  gate: FirmwareRolloutGateEntry[]
  createdAt: string
  updatedAt: string
}

export interface FirmwareRolloutTargetHealth {
  gate?: string
  reason?: string
  installedVersion?: string
  nodeOnline?: boolean
  commandStatus?: string
  taskStatus?: string
  lastSeen?: string
  alarmMetric?: string
  [key: string]: unknown
}

export interface FirmwareRolloutTarget {
  id: number
  rolloutId: number
  waveId: number
  tenantId: number
  nodeId: number
  previousFirmwareId: number
  taskId: number
  commandId: number
  status: FirmwareRolloutTargetStatus
  health: FirmwareRolloutTargetHealth
  updatedAt: string
}

export interface FirmwareRolloutRow {
  id: number
  tenantId: number
  firmwareId: number
  name: string
  policy: FirmwareRolloutPolicy
  status: FirmwareRolloutStatus
  currentWave: number
  totalWaves: number
  createdBy: number
  createdAt: string
  startedAt: string
  finishedAt: string
  pausedAt: string
  pauseReason: string
  lastError: string
  version: number
}

export interface FirmwareRolloutAggregate extends FirmwareRolloutRow {
  waves: FirmwareRolloutWave[]
  targets: FirmwareRolloutTarget[]
}

export interface FirmwareRolloutCreateRequest {
  name: string
  nodeIds: number[]
  policy: FirmwareRolloutPolicy
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
      batch: (body: { nodeIds: number[]; type: string; payload: string; timeoutSeconds: number; scheduledAt?: string }) =>
        client.post<{ batchId: number; commands: CommandRow[] }>('/commands/batch', body),
      cancel: (id: number) => client.put<CommandRow>(`/commands/${id}/cancel`, {}),
      cancelBatch: (batchId: number) => client.put<CommandRow[]>(`/commands/batch/${batchId}/cancel`, {}),
    },
    firmware: {
      list: (query?: QueryParams) => client.get<PageResult<FirmwareRow>>('/firmware', query),
      policy: () => client.get<FirmwarePolicy>('/firmware/policy'),
      remove: (id: number) => client.delete<void>(`/firmware/${id}`),
      tasks: (id: number) => client.get<FirmwareTaskRow[]>(`/firmware/${id}/tasks`),
      deploy: (id: number, nodeIds: number[], batchSize?: number) =>
        client.post<FirmwareDeployItem[] | { deployed: FirmwareDeployItem[]; deferredNodeIds: number[] }>(`/firmware/${id}/deploy`, batchSize ? { nodeIds, batchSize } : { nodeIds }),
      rollback: (id: number, nodeIds: number[]) =>
        client.post<Array<{ nodeId: number; fromFirmwareId: number; toFirmwareId: number; taskId: number; commandId: number }>>(`/firmware/${id}/rollback`, { nodeIds }),
      upload: (file: File, name: string, version: string, options?: FirmwareUploadOptions) => uploadFirmware(file, name, version, options),
      rollouts: (id: number, query?: QueryParams) => client.get<PageResult<FirmwareRolloutRow>>(`/firmware/${id}/rollouts`, query),
      rollout: (rolloutId: number) => client.get<FirmwareRolloutAggregate>(`/firmware/rollouts/${rolloutId}`),
      createRollout: (id: number, body: FirmwareRolloutCreateRequest) => client.post<FirmwareRolloutAggregate>(`/firmware/${id}/rollouts`, body),
      pauseRollout: (rolloutId: number, reason?: string) =>
        client.post<FirmwareRolloutAggregate>(`/firmware/rollouts/${rolloutId}/pause`, reason ? { reason } : {}),
      resumeRollout: (rolloutId: number) => client.post<FirmwareRolloutAggregate>(`/firmware/rollouts/${rolloutId}/resume`, {}),
      promoteRollout: (rolloutId: number) => client.post<FirmwareRolloutAggregate>(`/firmware/rollouts/${rolloutId}/promote`, {}),
      forcePromoteRollout: (rolloutId: number, reason: string) =>
        client.post<FirmwareRolloutAggregate>(`/firmware/rollouts/${rolloutId}/force-promote`, { reason }),
      abortRollout: (rolloutId: number) => client.post<FirmwareRolloutAggregate>(`/firmware/rollouts/${rolloutId}/abort`, {}),
      rollbackRollout: (rolloutId: number) => client.post<FirmwareRolloutAggregate>(`/firmware/rollouts/${rolloutId}/rollback`, {}),
    },
    ops: {
      summary: () => client.get<OpsSummary>('/ops/summary'),
      protocols: () => client.get<Record<string, unknown>>('/ops/protocols'),
    },
  }
}

function uploadFirmware(file: File, name: string, version: string, options: FirmwareUploadOptions = {}): Promise<FirmwareRow> {
  return new Promise((resolve, reject) => {
    const token = authSession.token()
    if (!token) {
      reject(new Error('登录已过期，请重新登录'))
      return
    }
    const query = new URLSearchParams({ name, version, fileName: file.name })
    if (options.tenantId) query.set('tenantId', String(options.tenantId))
    const request = new XMLHttpRequest()
    request.open('POST', `/api/firmware?${query.toString()}`)
    request.setRequestHeader('Authorization', `Bearer ${token}`)
    request.setRequestHeader('Content-Type', file.type || 'application/octet-stream')
    // 服务端把签名与密钥 ID 作为请求头读取；两段缺一会直接拒绝上传
    if (options.signature) request.setRequestHeader('X-Firmware-Signature', options.signature)
    if (options.signingKeyId) request.setRequestHeader('X-Firmware-Key-Id', options.signingKeyId)
    request.upload.onprogress = event => {
      if (event.lengthComputable && options.onProgress) options.onProgress(Math.round((event.loaded / event.total) * 100))
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
