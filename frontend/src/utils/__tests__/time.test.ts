import test from 'node:test'
import assert from 'node:assert/strict'
import { isLockActive } from '../time.ts'

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
