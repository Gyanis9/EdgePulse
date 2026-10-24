/* 后端 DATETIME 是 UTC 的 YYYY-MM-DD HH:MM:SS，不带时区；直接交给 Date 会按本地时间解释。 */
function utcMillis(value: string | null | undefined): number | null {
  if (!value) return null
  const text = value.trim()
  if (!text) return null
  const normalized = text.includes('T') ? text : text.replace(' ', 'T')
  const millis = Date.parse(normalized.endsWith('Z') ? normalized : `${normalized}Z`)
  return Number.isFinite(millis) ? millis : null
}

/** 锁定截止时刻仍在将来时才算锁定中；后端到期后不会主动清空 locked_until。 */
export function isLockActive(value: string | null | undefined, now = Date.now()): boolean {
  const deadline = utcMillis(value)
  return deadline !== null && deadline > now
}
