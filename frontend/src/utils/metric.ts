/** 遥测数值的展示格式：百分比保留最多两位小数，大数用紧凑单位，避免卡片溢出。 */
export function formatMetricValue(value: number | string | null | undefined, unit = ''): string {
  if (value === null || value === undefined || (typeof value === 'string' && value.trim() === '')) return '—'
  const numeric = typeof value === 'number' ? value : Number(value)
  if (!Number.isFinite(numeric)) return '—'
  const absolute = Math.abs(numeric)
  let options: Intl.NumberFormatOptions
  if (absolute >= 100000) {
    options = { notation: 'compact', maximumFractionDigits: 1 }
  } else if (absolute > 0 && absolute < 0.01) {
    options = { maximumSignificantDigits: 3 }
  } else if (unit === '%') {
    options = { minimumFractionDigits: 0, maximumFractionDigits: 2 }
  } else if (absolute >= 1000) {
    options = { maximumFractionDigits: 0 }
  } else {
    options = { maximumFractionDigits: 2 }
  }
  return new Intl.NumberFormat('zh-CN', options).format(numeric)
}
