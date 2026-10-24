<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { Button, DataTable, EmptyState, Field, Panel, Tag, TextInput, type Column } from '@/components/ui'
import { ApiError, deviceApi, type NodeConfigRevisionRow, type NodeConfigRow } from '@/api'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'
import PermissionButton from './PermissionButton.vue'

const props = defineProps<{ nodeId: number }>()
const emit = defineEmits<{ (e: 'updated'): void }>()

const REPORT_LABEL: Record<string, string> = { applied: '已应用', failed: '应用失败', pending: '待应用' }
const REPORT_TONE: Record<string, string> = { applied: 'ok', failed: 'danger', pending: 'info' }

const config = ref<NodeConfigRow | null>(null)
const revisions = ref<NodeConfigRevisionRow[]>([])
const revisionsTotal = ref(0)
const editor = ref('{}')
const note = ref('')
const loading = ref(false)
const saving = ref(false)
const rollingBack = ref<number | null>(null)
const error = ref('')
const conflict = ref(false)
const dirty = ref(false)

const hasConfig = computed(() => Boolean(config.value && config.value.desired.version > 0))
// 面板不分页，只展示最近一页修订；标题同时给出真实总数与已加载条数
const REVISION_WINDOW = 50
const revisionColumns: Column[] = [
  { key: 'version', title: '版本', width: 90 },
  { key: 'note', title: '备注', minWidth: 220 },
  { key: 'createdAt', title: '创建时间', width: 170 },
  { key: 'actions', title: '操作', width: 100, className: 'col-actions' },
]

function pretty(content: Record<string, unknown>) {
  return JSON.stringify(content ?? {}, null, 2)
}

function reportedLabel(status: string) {
  return REPORT_LABEL[status] || status || '未上报'
}

function reportedTone(status: string) {
  return REPORT_TONE[status] || 'neutral'
}

async function loadRevisions() {
  const page = await deviceApi.nodes.config.revisions(props.nodeId, { pageNum: 1, pageSize: REVISION_WINDOW })
  revisions.value = page.rows
  revisionsTotal.value = page.total
}

async function load(): Promise<boolean> {
  loading.value = true
  error.value = ''
  conflict.value = false
  try {
    const [current] = await Promise.all([deviceApi.nodes.config.get(props.nodeId), loadRevisions()])
    config.value = current
    editor.value = pretty(current.desired.content)
    dirty.value = false
    return true
  } catch (failure) {
    toastError(failure, '节点配置加载失败')
    return false
  } finally {
    loading.value = false
  }
}

function markDirty() {
  dirty.value = true
}

async function reload() {
  if (dirty.value) {
    pushToast('有未保存的配置修改，已保留草稿；保存或回滚后再刷新', 'info')
    return
  }
  await load()
}

async function save() {
  if (!config.value) {
    error.value = '配置尚未加载完成，请刷新后重试'
    return
  }
  error.value = ''
  conflict.value = false
  let content: unknown
  try {
    content = JSON.parse(editor.value)
  } catch {
    error.value = '配置内容必须是合法 JSON'
    return
  }
  if (!content || typeof content !== 'object' || Array.isArray(content)) {
    error.value = '配置内容必须是 JSON 对象，例如 {"interval":30}'
    return
  }
  saving.value = true
  try {
    const saved = await deviceApi.nodes.config.update(props.nodeId, {
      content: content as Record<string, unknown>,
      expectedVersion: config.value.desired.version,
      note: note.value.trim() || undefined,
    })
    config.value = saved
    editor.value = pretty(saved.desired.content)
    note.value = ''
    dirty.value = false
    pushToast('期望配置已下发，等待 Agent 上报')
    emit('updated')
    await loadRevisions()
  } catch (failure) {
    if (failure instanceof ApiError && (failure.status === 409 || failure.code === 409)) {
      conflict.value = true
      error.value = '配置版本已被其他管理员更新，请刷新后重新编辑'
    } else {
      toastError(failure, '配置下发失败')
    }
  } finally {
    saving.value = false
  }
}

async function refreshAfterConflict() {
  if (await load()) pushToast('已加载服务端最新配置')
}

async function rollback(revision: NodeConfigRevisionRow) {
  const ok = await confirm({
    title: '回滚配置',
    message: `确认把期望配置回滚到版本 v${revision.version} 吗？这会生成一个新的期望版本，并等待 Agent 上报。`,
    confirmText: '回滚',
  })
  if (!ok) return
  rollingBack.value = revision.id
  try {
    const rolled = await deviceApi.nodes.config.rollback(props.nodeId, {
      revisionId: revision.id,
      note: note.value.trim() || `回滚到版本 v${revision.version}`,
    })
    config.value = rolled
    editor.value = pretty(rolled.desired.content)
    dirty.value = false
    pushToast(`已回滚到 v${revision.version}，等待 Agent 上报`)
    emit('updated')
    await loadRevisions()
  } catch (failure) {
    toastError(failure, '配置回滚失败')
  } finally {
    rollingBack.value = null
  }
}

onMounted(load)

defineExpose({ reload })
</script>

<template>
  <div class="config-panel">
    <div v-if="config && !config.inSync" class="drift-banner" role="status">
      <Tag tone="danger" dot>配置漂移</Tag>
      <span>期望版本 v{{ config.desired.version }}，Agent 实际版本 v{{ config.reported.version }}。</span>
      <Button variant="text" size="sm" icon="refresh" @click="reload">刷新</Button>
    </div>

    <EmptyState v-if="config && !hasConfig && !loading" title="尚未下发配置" desc="在下方编辑 JSON 对象，下发后右侧会显示 Agent 的实际上报。" icon="sliders" />

    <div v-else class="config-compare">
      <Panel title="期望配置" :meta="config ? `v${config.desired.version}` : ''">
        <div class="panel-body config-stack">
          <div v-if="config" class="config-meta">
            <span class="mono">{{ config.desired.updatedAt || '—' }}</span>
            <span>操作人 #{{ config.desired.updatedBy || '—' }}</span>
            <code v-if="config.desired.hash">hash {{ config.desired.hash.slice(0, 16) }}</code>
          </div>
          <pre class="code-block config-block">{{ config ? pretty(config.desired.content) : '{}' }}</pre>
        </div>
      </Panel>

      <Panel title="实际上报" :meta="config ? `v${config.reported.version}` : ''">
        <div class="panel-body config-stack">
          <div v-if="config" class="config-meta">
            <Tag :tone="reportedTone(config.reported.status)" dot>{{ reportedLabel(config.reported.status) }}</Tag>
            <span class="mono">{{ config.reported.at || '—' }}</span>
            <code v-if="config.reported.hash">hash {{ config.reported.hash.slice(0, 16) }}</code>
          </div>
          <p v-if="config?.reported.message" class="faint" style="margin: 0">Agent 说明：{{ config.reported.message }}</p>
          <pre class="code-block config-block">{{ config ? pretty(config.reported.content) : '{}' }}</pre>
        </div>
      </Panel>
    </div>

    <Panel title="下发期望配置" :meta="config ? `当前 v${config.desired.version}` : ''">
      <div class="panel-body config-stack">
        <Field label="配置内容（JSON 对象）" required hint="保存时会携带当前期望版本，版本被他人推进后需要刷新再改">
          <TextInput v-model="editor" multiline :rows="12" class="config-editor" placeholder='{"interval":30}' @input="markDirty" />
        </Field>
        <Field label="变更备注" hint="选填；会记入配置修订历史">
          <TextInput v-model="note" placeholder="例如 调整采集间隔" />
        </Field>
        <div class="token-actions">
          <PermissionButton perm="device:node:write" variant="primary" icon="upload" label="下发配置" :loading="saving" @click="save" />
        </div>
        <p v-if="error" class="form-error">{{ error }}</p>
        <div v-if="conflict">
          <Button variant="secondary" size="sm" icon="refresh" @click="refreshAfterConflict">刷新配置</Button>
        </div>
      </div>
    </Panel>

    <Panel title="历史版本" :meta="revisionsTotal > revisions.length ? `共 ${revisionsTotal} 条，显示最近 ${revisions.length} 条` : `${revisionsTotal} 条`">
      <DataTable :columns="revisionColumns" :data="revisions" :loading="loading" :min-width="620" empty-text="还没有配置修订">
        <template #cell-version="{ row }"><span class="mono">v{{ row.version }}</span></template>
        <template #cell-note="{ row }"><span :class="row.note ? 'muted' : 'faint'">{{ row.note || '—' }}</span></template>
        <template #cell-createdAt="{ row }"><span class="mono soft">{{ row.createdAt }}</span></template>
        <template #cell-actions="{ row }">
          <PermissionButton
            perm="device:node:write"
            variant="text"
            size="sm"
            icon="refresh"
            label="回滚"
            :loading="rollingBack === row.id"
            @click="rollback(row)"
          />
        </template>
      </DataTable>
    </Panel>
  </div>
</template>
