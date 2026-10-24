import test from 'node:test'
import assert from 'node:assert/strict'
import { formatMetricValue } from '../metric.ts'

test('percent metrics keep two decimals without overflowing cards', () => {
  assert.equal(formatMetricValue(2.37014624306, '%'), '2.37')
  assert.equal(formatMetricValue(12.6640142986, '%'), '12.66')
  assert.equal(formatMetricValue(22.2357344151, '%'), '22.24')
})

test('large values use grouping or compact notation', () => {
  assert.equal(formatMetricValue(1234.5), '1,235')
  assert.match(formatMetricValue(123456), /万/)
})

test('small and invalid values are bounded', () => {
  assert.equal(formatMetricValue(0.001234), '0.00123')
  assert.equal(formatMetricValue('not-a-number'), '—')
  assert.equal(formatMetricValue(Number.POSITIVE_INFINITY), '—')
  assert.equal(formatMetricValue(null), '—')
})
