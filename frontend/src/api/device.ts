import type { HttpClient, QueryParams } from './http'
import type { PageResult } from './system'

export interface NodeRow {
  id: number
  name: string
  status: 'online' | 'offline'
  agentVersion: string
  installedFirmwareVersion?: string
  certFingerprint: string
  labels: Record<string, string> | null
  lastSeen: string
  groupId: number
  groupName: string
  version: number
}

export interface NodeDetailRow extends NodeRow {
  certNotAfter: string
  certRevokedAt: string
  previousCertFingerprint?: string
  previousCertNotAfter?: string
  desiredConfigVersion: number
  reportedConfigVersion: number
  configInSync: boolean
}

export interface NodeWriteRequest {
  name?: string
  labels?: Record<string, string> | null
  groupId?: number
  version?: number
  tenantId?: number
}

export interface EnrollmentTokenRow {
  id: number
  nodeName: string
  action: 'enroll' | 'rebind'
  status: 'active' | 'used' | 'expired' | 'revoked'
  expiresAt: string
  createdAt: string
}

export interface EnrollmentTokenCreated extends EnrollmentTokenRow {
  token: string
}

export interface EnrollmentTokenWriteRequest {
  action?: 'enroll' | 'rebind'
  nodeName?: string
  expiresInSeconds?: number
  tenantId?: number
}

export interface NodeConfigDesired {
  version: number
  content: Record<string, unknown>
  hash: string
  updatedAt: string
  updatedBy: number
}

export interface NodeConfigReported {
  version: number
  content: Record<string, unknown>
  hash: string
  status: string
  message: string
  at: string
}

export interface NodeConfigRow {
  desired: NodeConfigDesired
  reported: NodeConfigReported
  inSync: boolean
}

export interface NodeConfigRevisionRow {
  id: number
  version: number
  content: Record<string, unknown>
  note: string
  createdAt: string
}

export interface NodeConfigUpdateRequest {
  content: Record<string, unknown>
  expectedVersion: number
  note?: string
}

export interface NodeConfigRollbackRequest {
  revisionId: number
  note?: string
}

export function createDeviceApi(client: HttpClient) {
  return {
    nodes: {
      list: (query?: QueryParams) => client.get<PageResult<NodeRow>>('/nodes', query),
      get: (id: number) => client.get<NodeDetailRow>(`/nodes/${id}`),
      create: (body: NodeWriteRequest) => client.post<NodeRow>('/nodes', body),
      update: (id: number, body: NodeWriteRequest) => client.put<NodeRow>(`/nodes/${id}`, body),
      remove: (id: number) => client.delete<void>(`/nodes/${id}`),
      revokeCertificate: (id: number) => client.post<NodeRow>(`/nodes/${id}/certificate/revoke`, {}),
      config: {
        get: (id: number) => client.get<NodeConfigRow>(`/nodes/${id}/config`),
        update: (id: number, body: NodeConfigUpdateRequest) => client.put<NodeConfigRow>(`/nodes/${id}/config`, body),
        revisions: (id: number, query?: QueryParams) => client.get<PageResult<NodeConfigRevisionRow>>(`/nodes/${id}/config/revisions`, query),
        rollback: (id: number, body: NodeConfigRollbackRequest) => client.post<NodeConfigRow>(`/nodes/${id}/config/rollback`, body),
      },
    },
    enrollmentTokens: {
      // 注册令牌是租户级资源，路径不带 /nodes 前缀
      list: (query?: QueryParams) => client.get<EnrollmentTokenRow[]>('/enrollment-tokens', query),
      create: (body: EnrollmentTokenWriteRequest) => client.post<EnrollmentTokenCreated>('/enrollment-tokens', body),
      revoke: (id: number) => client.delete<void>(`/enrollment-tokens/${id}`),
    },
  }
}
