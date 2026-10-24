import test from 'node:test'
import assert from 'node:assert/strict'
import { formatLocalDateTime, formatLocalTime, isLockActive } from '../time.ts'

test('lock is active only before the UTC deadline', () => {
  const deadline = Date.UTC(2026, 0, 1, 0, 0, 0)
  assert.equal(isLockActive('2026-01-01 00:00:00', deadline - 1), true)
  assert.equal(isLockActive('2026-01-01 00:00:00', deadline), false)
  assert.equal(isLockActive('2026-01-01 00:00:00', deadline + 1), false)
})

test('empty or malformed lock timestamps are not active', () => {
  assert.equal(isLockActive(''), false)
  assert.equal(isLockActive('   '), false)
  assert.equal(isLockActive(undefined), false)
  assert.equal(isLockActive('not-a-date'), false)
})

test('UTC datetime text is rendered in the requested timezone', () => {
  assert.equal(formatLocalDateTime('2026-10-24 16:29:56', 'Asia/Shanghai'), '2026-10-25 00:29:56')
  assert.equal(formatLocalDateTime('2026-10-24 16:29:56', 'UTC'), '2026-10-24 16:29:56')
  assert.equal(formatLocalTime('2026-10-24 16:29:56', 'Asia/Shanghai'), '00:29')
})

test('timezone suffix is honored without shifting twice', () => {
  assert.equal(formatLocalDateTime('2026-10-24T16:29:56Z', 'Asia/Shanghai'), '2026-10-25 00:29:56')
  assert.equal(formatLocalDateTime('2026-10-25T00:29:56+08:00', 'UTC'), '2026-10-24 16:29:56')
})

test('empty or malformed display timestamps render as a dash', () => {
  assert.equal(formatLocalDateTime(''), '—')
  assert.equal(formatLocalDateTime('   '), '—')
  assert.equal(formatLocalDateTime(undefined), '—')
  assert.equal(formatLocalDateTime('not-a-date'), '—')
  assert.equal(formatLocalTime('not-a-date'), '—')
})
