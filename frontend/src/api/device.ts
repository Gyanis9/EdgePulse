import type { HttpClient, QueryParams } from './http'
import type { PageResult } from './system'

export interface NodeRow {
  id: number
  name: string
  status: 'online' | 'offline'
  agentVersion: string
  certFingerprint: string
  labels: Record<string, string> | null
  lastSeen: string
  groupId: number
  groupName: string
  version: number
}

export interface NodeWriteRequest {
  name?: string
  labels?: Record<string, string> | null
  groupId?: number
  version?: number
}

export function createDeviceApi(client: HttpClient) {
  return {
    nodes: {
      list: (query?: QueryParams) => client.get<PageResult<NodeRow>>('/nodes', query),
      create: (body: NodeWriteRequest) => client.post<NodeRow>('/nodes', body),
      update: (id: number, body: NodeWriteRequest) => client.put<NodeRow>(`/nodes/${id}`, body),
      remove: (id: number) => client.delete<void>(`/nodes/${id}`),
    },
  }
}
