import test from 'node:test'
import assert from 'node:assert/strict'
import { createHttpClient } from '../http.ts'
import { createDeviceApi } from '../device.ts'

test('device api maps node list and CRUD calls to backend endpoints', async () => {
  const calls: Array<{ url: string; method: string; body?: string }> = []
  const fetcher = async (url: string | URL | Request, init?: RequestInit) => {
    calls.push({ url: String(url), method: init?.method ?? 'GET', body: init?.body as string | undefined })
    return new Response(JSON.stringify({ code: 200, message: 'success', data: { id: 7, rows: [], total: 0 }, traceId: 't' }), {
      status: 200,
      headers: { 'content-type': 'application/json' },
    })
  }
  const api = createDeviceApi(createHttpClient({ baseUrl: '/api', tokenProvider: () => 'token', fetcher }))

  await api.nodes.list({ pageNum: 2, pageSize: 20, keyword: 'edge', status: 'online' })
  await api.nodes.create({ name: 'edge-1', labels: { site: 'shanghai' } })
  await api.nodes.update(7, { name: 'edge-1b', version: 1 })
  await api.nodes.remove(7)

  assert.equal(calls[0].url, '/api/nodes?pageNum=2&pageSize=20&keyword=edge&status=online')
  assert.equal(calls[1].method, 'POST')
  assert.deepEqual(JSON.parse(calls[1].body ?? '{}'), { name: 'edge-1', labels: { site: 'shanghai' } })
  assert.equal(calls[2].method, 'PUT')
  assert.equal(calls[2].url, '/api/nodes/7')
  assert.equal(calls[3].method, 'DELETE')
  assert.equal(calls[3].url, '/api/nodes/7')
})

test('device api maps node detail, certificate, enrollment and config endpoints', async () => {
  const calls: Array<{ url: string; method: string; body?: string }> = []
  const fetcher = async (url: string | URL | Request, init?: RequestInit) => {
    calls.push({ url: String(url), method: init?.method ?? 'GET', body: init?.body as string | undefined })
    return new Response(JSON.stringify({ code: 200, message: 'ok', data: { id: 7, rows: [], total: 0 }, traceId: 't' }), {
      status: 200,
      headers: { 'content-type': 'application/json' },
    })
  }
  const api = createDeviceApi(createHttpClient({ baseUrl: '/api', tokenProvider: () => 'token', fetcher }))

  await api.nodes.get(7)
  await api.nodes.revokeCertificate(7)
  await api.enrollmentTokens.create({ action: 'enroll', expiresInSeconds: 3600 })
  await api.nodes.config.update(7, { content: { interval: 30 }, expectedVersion: 0 })

  assert.equal(calls[0].url, '/api/nodes/7')
  assert.equal(calls[1].method, 'POST')
  assert.equal(calls[1].url, '/api/nodes/7/certificate/revoke')
  assert.equal(calls[2].method, 'POST')
  assert.equal(calls[2].url, '/api/enrollment-tokens')
  assert.equal(calls[3].method, 'PUT')
  assert.equal(calls[3].url, '/api/nodes/7/config')
  assert.deepEqual(JSON.parse(calls[3].body ?? '{}'), { content: { interval: 30 }, expectedVersion: 0 })
})

test('device api maps enrollment listing and config revision endpoints', async () => {
  const calls: Array<{ url: string; method: string; body?: string }> = []
  const fetcher = async (url: string | URL | Request, init?: RequestInit) => {
    calls.push({ url: String(url), method: init?.method ?? 'GET', body: init?.body as string | undefined })
    return new Response(JSON.stringify({ code: 200, message: 'ok', data: { id: 7, rows: [], total: 0 }, traceId: 't' }), {
      status: 200,
      headers: { 'content-type': 'application/json' },
    })
  }
  const api = createDeviceApi(createHttpClient({ baseUrl: '/api', tokenProvider: () => 'token', fetcher }))

  await api.enrollmentTokens.list({ status: 'active' })
  await api.enrollmentTokens.revoke(5)
  await api.nodes.config.get(7)
  await api.nodes.config.revisions(7, { pageNum: 1, pageSize: 20 })
  await api.nodes.config.rollback(7, { revisionId: 3, note: '回退到稳定版本' })

  assert.equal(calls[0].url, '/api/enrollment-tokens?status=active')
  assert.equal(calls[1].method, 'DELETE')
  assert.equal(calls[1].url, '/api/enrollment-tokens/5')
  assert.equal(calls[2].url, '/api/nodes/7/config')
  assert.equal(calls[3].url, '/api/nodes/7/config/revisions?pageNum=1&pageSize=20')
  assert.equal(calls[4].method, 'POST')
  assert.equal(calls[4].url, '/api/nodes/7/config/rollback')
  assert.deepEqual(JSON.parse(calls[4].body ?? '{}'), { revisionId: 3, note: '回退到稳定版本' })
})
