import test from 'node:test'
import assert from 'node:assert/strict'
import { createHttpClient } from '../http.ts'
import { createFleetApi } from '../fleet.ts'

test('fleet api maps telemetry alarm command firmware and ops endpoints', async () => {
  const calls: Array<{ url: string; method: string; body?: string }> = []
  const fetcher = async (url: string | URL | Request, init?: RequestInit) => {
    calls.push({ url: String(url), method: init?.method ?? 'GET', body: init?.body as string | undefined })
    return new Response(JSON.stringify({ code: 200, message: 'ok', data: { id: 3, rows: [], total: 0, openCount: 2 }, traceId: 't' }), {
      status: 200,
      headers: { 'content-type': 'application/json' },
    })
  }
  const api = createFleetApi(createHttpClient({ baseUrl: '/api', tokenProvider: () => 'token', fetcher }))

  await api.nodeGroups.tree()
  await api.nodeGroups.create({ name: '华东', parentId: 0 })
  await api.telemetry.latest(7)
  await api.telemetry.series(7, 'cpu.usage', '2026-10-20 00:00:00', '2026-10-20 23:59:59')
  await api.alarms.list({ status: 'open', level: 'critical' })
  await api.alarms.ack(9)
  await api.alarms.resolve(9)
  await api.alarms.createRule({ name: 'CPU', nodeId: 0, metric: 'cpu.usage', comparison: '>', threshold: 90, level: 'critical', enabled: true, cooldownSeconds: 300 })
  await api.commands.create({ nodeId: 7, type: 'reboot', payload: '{}', timeoutSeconds: 30 })
  await api.commands.cancel(11)
  await api.firmware.deploy(5, [7])
  await api.firmware.tasks(5)
  await api.ops.summary()

  assert.equal(calls[0].url, '/api/node-groups')
  assert.deepEqual(JSON.parse(calls[1].body ?? '{}'), { name: '华东', parentId: 0 })
  assert.equal(calls[2].url, '/api/telemetry/latest?nodeId=7')
  assert.equal(calls[3].url, '/api/telemetry/series?nodeId=7&metric=cpu.usage&from=2026-10-20+00%3A00%3A00&to=2026-10-20+23%3A59%3A59')
  assert.equal(calls[4].url, '/api/alarms?status=open&level=critical')
  assert.equal(calls[5].url, '/api/alarms/9/ack')
  assert.equal(calls[6].url, '/api/alarms/9/resolve')
  assert.equal(calls[7].url, '/api/alarm-rules')
  assert.equal(calls[8].url, '/api/commands')
  assert.equal(calls[9].url, '/api/commands/11/cancel')
  assert.equal(calls[10].url, '/api/firmware/5/deploy')
  assert.deepEqual(JSON.parse(calls[10].body ?? '{}'), { nodeIds: [7] })
  assert.equal(calls[11].url, '/api/firmware/5/tasks')
  assert.equal(calls[12].url, '/api/ops/summary')
})
