import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { ICON_PATHS, MENU_ICON_OPTIONS } from '../icons.ts'

const seedFiles = [
  new URL('../../../../migrations/V2__seed.sql', import.meta.url),
  new URL('../../../../migrations/V4__management_domain.sql', import.meta.url),
  new URL('../../../../migrations/V8__multi_tenancy.sql', import.meta.url),
  new URL('../../../../migrations/V9__notification_channels.sql', import.meta.url),
  new URL('../../../../migrations/V19__enterprise_identity.sql', import.meta.url),
]

function seedMenuIcons(): string[] {
  const icons = new Set<string>()
  for (const file of seedFiles) {
    const sql = readFileSync(file, 'utf8')
    for (const match of sql.matchAll(/\(\d+,\s*\d+,\s*'[^']*',\s*'(?:menu|dir|button)',\s*'([^']*)'/g)) {
      icons.add(match[1])
    }
  }
  return [...icons].sort()
}

test('all seeded menu icons resolve to an icon definition', () => {
  const missing = seedMenuIcons().filter(name => !ICON_PATHS[name])
  assert.deepEqual(missing, [], `缺少菜单图标路径：${missing.join(', ')}`)
})

test('menu icon options are renderable', () => {
  const missing = MENU_ICON_OPTIONS.filter(name => !ICON_PATHS[name])
  assert.deepEqual(missing, [], `菜单可选图标未登记：${missing.join(', ')}`)
})

test('every icon definition is non-empty', () => {
  const empty = Object.entries(ICON_PATHS)
    .filter(([, definition]) => typeof definition === 'string' ? definition.trim() === '' : definition.length === 0)
    .map(([name]) => name)
  assert.deepEqual(empty, [])
})
