<script setup lang="ts">
import { onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { Button, Field, Modal, Tag, TextInput, type Column } from '@/components/ui'
import { FormDialog, ListPage, PermissionButton } from '@/components/business'
import { systemApi, type ApiTokenCreated, type ApiTokenRow } from '@/api'
import { useListPage } from '@/composables/list'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'
import { copyText } from '@/utils/clipboard'
import { formatLocalDateTime } from '@/utils/time'
import { maxLength, required, validate } from '@/utils/validate'

const list = useListPage<ApiTokenRow, { keyword: string }>({
  query: { keyword: '' },
  fetch: async params => {
    // 后端一次返回当前租户全部令牌，关键词与分页都在本地完成
    const keyword = String(params.keyword ?? '').trim().toLowerCase()
    const all = await systemApi.apiTokens.list()
    const filtered = keyword
      ? all.filter(row => row.name.toLowerCase().includes(keyword)
        || row.prefix.toLowerCase().includes(keyword)
        || row.scopes.some(scope => scope.toLowerCase().includes(keyword)))
      : all
    const pageNum = Number(params.pageNum ?? 1)
    const pageSize = Number(params.pageSize ?? 10)
    return {
      rows: filtered.slice((pageNum - 1) * pageSize, pageNum * pageSize),
      total: filtered.length,
      pageNum,
      pageSize,
    }
  },
})

const columns: Column[] = [
  { key: 'name', title: '令牌名称', minWidth: 160 },
  { key: 'prefix', title: '前缀', width: 150 },
  { key: 'scopes', title: '权限范围', minWidth: 260 },
  { key: 'expiresAt', title: '到期时间', width: 170 },
  { key: 'lastUsedAt', title: '最近使用', width: 170 },
  { key: 'status', title: '状态', width: 96 },
  { key: 'createdAt', title: '创建时间', width: 170 },
  { key: 'actions', title: '操作', width: 110, className: 'col-actions' },
]

/** 后端时间不带时区，按 UTC 解析；无法解析时返回 null */
function expiryMillis(value: string): number | null {
  if (!value) return null
  const normalized = value.includes('T') ? value : value.replace(' ', 'T')
  const millis = Date.parse(/(?:[zZ]|[+-]\d{2}:?\d{2})$/.test(normalized) ? normalized : `${normalized}Z`)
  return Number.isFinite(millis) ? millis : null
}

/** 状态判定用的当前时刻：页面停留时每 30 秒推进一次，回到前台也会刷新 */
const now = ref(Date.now())
let statusTimer: number | undefined

function refreshStatus() {
  now.value = Date.now()
}

function onVisibilityChange() {
  if (!document.hidden) refreshStatus()
}

onMounted(() => {
  statusTimer = window.setInterval(refreshStatus, 30_000)
  document.addEventListener('visibilitychange', onVisibilityChange)
})

onBeforeUnmount(() => {
  if (statusTimer !== undefined) window.clearInterval(statusTimer)
  document.removeEventListener('visibilitychange', onVisibilityChange)
})

function statusOf(row: ApiTokenRow) {
  if (row.revokedAt) return { label: '已吊销', tone: 'danger' }
  const expires = expiryMillis(row.expiresAt)
  if (expires === null) return { label: '到期未知', tone: 'neutral' }
  if (expires <= now.value) return { label: '已过期', tone: 'neutral' }
  return { label: '有效', tone: 'ok' }
}

/** 到期时间无法解析时按不可用处理，轮换入口一并隐藏 */
function isActive(row: ApiTokenRow) {
  if (row.revokedAt) return false
  const expires = expiryMillis(row.expiresAt)
  return expires !== null && expires > now.value
}

interface TokenForm {
  name: string
  scopesText: string
  expiryDays: number | null
}
const emptyForm = (): TokenForm => ({ name: '', scopesText: '', expiryDays: 30 })
const dialog = ref(false)
const saving = ref(false)
const form = reactive<TokenForm>(emptyForm())
const errors = ref<Partial<Record<keyof TokenForm, string>>>({})

const secretOpen = ref(false)
const secretTitle = ref('令牌已创建')
const secret = ref<ApiTokenCreated | null>(null)

function parseScopes(text: string) {
  const seen = new Set<string>()
  const scopes: string[] = []
  for (const part of text.split(/[,，\s]+/)) {
    const scope = part.trim()
    if (!scope || seen.has(scope)) continue
    seen.add(scope)
    scopes.push(scope)
  }
  return scopes
}

function scopeRule(value: unknown) {
  return String(value ?? '').trim() && parseScopes(String(value)).length ? undefined : '请输入至少一个权限码'
}

function expiryRule(value: unknown) {
  const days = Number(value)
  return Number.isInteger(days) && days >= 1 && days <= 365 ? undefined : '有效期必须是 1 到 365 之间的整数天'
}

function openCreate() {
  Object.assign(form, emptyForm())
  errors.value = {}
  dialog.value = true
}

function showSecret(created: ApiTokenCreated, title: string) {
  secret.value = created
  secretTitle.value = title
  secretOpen.value = true
}

function closeSecret() {
  // 明文只保留到对话框关闭，不落本地存储
  secretOpen.value = false
  secret.value = null
}

async function copySecret() {
  if (!secret.value) return
  if (await copyText(secret.value.token)) pushToast('令牌已复制')
  else pushToast('复制失败，请手动选中令牌文本', 'danger')
}

async function submit() {
  // 隐藏提交按钮与输入框回车都会再进这里；保存中或对话框已关闭时都不得再次创建
  if (saving.value || !dialog.value) return
  errors.value = validate(form, {
    name: [required('请输入令牌名称'), maxLength(64, '令牌名称不能超过 64 个字符')],
    scopesText: [scopeRule],
    expiryDays: [expiryRule],
  })
  if (Object.keys(errors.value).length) return
  saving.value = true
  try {
    const created = await systemApi.apiTokens.create({
      name: form.name.trim(),
      scopes: parseScopes(form.scopesText),
      expiresInSeconds: Number(form.expiryDays) * 86400,
    })
    dialog.value = false
    showSecret(created, '令牌已创建')
    // 新令牌在列表首行，回到第一页再拉取
    list.page.value = 1
    await list.load()
  } catch (error) {
    toastError(error, '令牌创建失败')
  } finally {
    saving.value = false
  }
}

/** 单行轮换 / 吊销的进行中标记：确认框与按钮都不可重复触发 */
const rowBusy = ref(new Set<number>())

function beginRow(row: ApiTokenRow) {
  if (rowBusy.value.has(row.id)) return false
  const next = new Set(rowBusy.value)
  next.add(row.id)
  rowBusy.value = next
  return true
}

function endRow(row: ApiTokenRow) {
  const next = new Set(rowBusy.value)
  next.delete(row.id)
  rowBusy.value = next
}

async function rotate(row: ApiTokenRow) {
  if (!beginRow(row)) return
  try {
    const ok = await confirm({
      title: '轮换令牌',
      message: `确认轮换「${row.name}」吗？新明文只显示一次，旧令牌在 1 小时内仍可用，之后立即失效。`,
      confirmText: '轮换',
      danger: true,
    })
    if (!ok) return
    const created = await systemApi.apiTokens.rotate(row.id)
    showSecret(created, '令牌已轮换')
    await list.load()
  } catch (error) {
    toastError(error, '令牌轮换失败')
  } finally {
    endRow(row)
  }
}

async function revoke(row: ApiTokenRow) {
  if (!beginRow(row)) return
  try {
    const ok = await confirm({
      title: '吊销令牌',
      message: `确认吊销「${row.name}」吗？使用该令牌的脚本会立即失效，且无法恢复。`,
      confirmText: '吊销',
      danger: true,
    })
    if (!ok) return
    await systemApi.apiTokens.remove(row.id)
    pushToast('令牌已吊销')
    await list.load()
  } catch (error) {
    toastError(error, '令牌吊销失败')
  } finally {
    endRow(row)
  }
}
</script>

<template>
  <ListPage title="API 令牌" desc="供脚本与外部系统调用开放接口；明文只在创建或轮换时显示一次，平台不保存明文。" table-title="令牌列表" :list="list" :columns="columns" :min-width="1180" empty-text="还没有 API 令牌">
    <template #actions>
      <PermissionButton perm="system:token:add" variant="primary" icon="plus" @click="openCreate">新增令牌</PermissionButton>
    </template>

    <template #search>
      <Field label="关键词" wide>
        <TextInput v-model="list.query.keyword" icon="search" placeholder="名称 / 前缀 / 权限码" clearable @enter="list.search()" />
      </Field>
    </template>

    <template #cell-name="{ row }">
      <div class="cell-main">
        <div style="min-width: 0">
          <div class="cell-title ellipsis">{{ row.name }}</div>
          <div class="cell-sub mono">#{{ row.id }}</div>
        </div>
      </div>
    </template>
    <template #cell-prefix="{ row }"><span class="mono soft">{{ row.prefix }}…</span></template>
    <template #cell-scopes="{ row }">
      <span class="scope-tags">
        <Tag v-for="scope in row.scopes" :key="scope" tone="info">{{ scope }}</Tag>
      </span>
    </template>
    <template #cell-expiresAt="{ row }"><span class="tnum soft">{{ formatLocalDateTime(row.expiresAt) }}</span></template>
    <template #cell-lastUsedAt="{ row }">
      <span v-if="row.lastUsedAt" class="tnum soft">{{ formatLocalDateTime(row.lastUsedAt) }}</span>
      <span v-else class="muted">从未使用</span>
    </template>
    <template #cell-status="{ row }"><Tag :tone="statusOf(row).tone" dot>{{ statusOf(row).label }}</Tag></template>
    <template #cell-createdAt="{ row }"><span class="tnum soft">{{ formatLocalDateTime(row.createdAt) }}</span></template>
    <template #cell-actions="{ row }">
      <div class="row-actions">
        <PermissionButton v-if="isActive(row)" perm="system:token:edit" icon-only icon="refresh" label="轮换" :disabled="rowBusy.has(row.id)" @click="rotate(row)" />
        <PermissionButton v-if="!row.revokedAt" perm="system:token:remove" icon-only icon="ban" label="吊销" danger :disabled="rowBusy.has(row.id)" @click="revoke(row)" />
      </div>
    </template>

    <FormDialog :open="dialog" title="新增 API 令牌" sub="令牌只能获得你当前权限的子集；明文仅显示一次。" :saving="saving" @close="dialog = false" @submit="submit">
      <Field label="令牌名称" required :error="errors.name" hint="用于识别调用方，如 ci-pipeline">
        <TextInput v-model="form.name" placeholder="例如 ci-pipeline" :invalid="!!errors.name" autocomplete="off" />
      </Field>
      <Field label="有效期（天）" required :error="errors.expiryDays" hint="允许 1–365 天；默认 30 天">
        <TextInput v-model.number="form.expiryDays" type="number" :invalid="!!errors.expiryDays" />
      </Field>
      <Field label="权限范围" required :error="errors.scopesText" hint="逗号分隔的权限码，必须是你当前拥有的权限；如 device:node:list, device:command:send" span>
        <TextInput v-model="form.scopesText" multiline :rows="3" placeholder="device:node:list, device:telemetry:read" :invalid="!!errors.scopesText" />
      </Field>
    </FormDialog>

    <Modal :open="secretOpen" :title="secretTitle" sub="明文只显示这一次；关闭后无法再次查看。" :width="620" @close="closeSecret">
      <div v-if="secret" class="token-once">
        <div>
          <div class="section-label">请立即复制</div>
          <p class="faint" style="margin: 4px 0 0">平台不保存明文。关闭对话框后只能重新轮换获取新令牌。</p>
        </div>
        <pre class="code-block token-value">{{ secret.token }}</pre>
        <div class="token-actions">
          <Button variant="primary" size="sm" icon="copy" @click="copySecret">复制令牌</Button>
          <Tag tone="ok" dot>有效至 {{ formatLocalDateTime(secret.expiresAt) }}</Tag>
        </div>
        <p v-if="secret.previousValidUntil" class="faint" style="margin: 0">
          旧令牌仍可用至 {{ formatLocalDateTime(secret.previousValidUntil) }}，过时后自动失效。
        </p>
      </div>
      <template #footer>
        <Button variant="ghost" @click="closeSecret">我已保存</Button>
      </template>
    </Modal>
  </ListPage>
</template>

<style scoped>
.scope-tags {
  display: inline-flex;
  flex-wrap: wrap;
  gap: 4px;
}
</style>
