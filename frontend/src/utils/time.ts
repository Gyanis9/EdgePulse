/* 后端 DATETIME 是 UTC 的 YYYY-MM-DD HH:MM:SS，不带时区；直接交给 Date 会按本地时间解释。 */
const TIMEZONE_SUFFIX = /(?:[zZ]|[+-]\d{2}:?\d{2})$/

function utcMillis(value: string | null | undefined): number | null {
  if (!value) return null
  const text = value.trim()
  if (!text) return null
  const normalized = text.includes('T') ? text : text.replace(' ', 'T')
  const millis = Date.parse(TIMEZONE_SUFFIX.test(normalized) ? normalized : `${normalized}Z`)
  return Number.isFinite(millis) ? millis : null
}

function localParts(millis: number, timeZone: string | undefined): Record<string, string> {
  const formatter = new Intl.DateTimeFormat('en-CA', {
    timeZone,
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hourCycle: 'h23',
  })
  return Object.fromEntries(formatter.formatToParts(new Date(millis)).map(part => [part.type, part.value]))
}

function display(value: string | null | undefined, timeZone: string | undefined, withSeconds: boolean): string {
  const millis = utcMillis(value)
  if (millis === null) return '—'
  const parts = localParts(millis, timeZone)
  const clock = `${parts.hour}:${parts.minute}${withSeconds ? `:${parts.second}` : ''}`
  return withSeconds ? `${parts.year}-${parts.month}-${parts.day} ${clock}` : clock
}

/** 把后端的 UTC 时间按浏览器本地时区显示成完整的日期时间。 */
export function formatLocalDateTime(value: string | null | undefined, timeZone?: string): string {
  return display(value, timeZone, true)
}

/** 把后端的 UTC 时间按浏览器本地时区显示成时分。 */
export function formatLocalTime(value: string | null | undefined, timeZone?: string): string {
  return display(value, timeZone, false)
}

/** 锁定截止时刻仍在将来时才算锁定中；后端到期后不会主动清空 locked_until。 */
export function isLockActive(value: string | null | undefined, now = Date.now()): boolean {
  const deadline = utcMillis(value)
  return deadline !== null && deadline > now
}
