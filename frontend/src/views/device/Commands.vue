<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { Drawer, Field, Select, StatusTag, Tag, TextInput, type Column } from '@/components/ui'
import { FormDialog, ListPage, PermissionButton } from '@/components/business'
import { deviceApi, fleetApi, type CommandRow, type NodeRow } from '@/api'
import { useListPage } from '@/composables/list'
import { useNodeSocket } from '@/composables/nodeSocket'
import { pushToast, toastError } from '@/composables/toast'

const STATUSES = [
  { label: '全部状态', value: '' },
  { label: '待执行', value: 'pending' },
  { label: '执行中', value: 'running' },
  { label: '已成功', value: 'succeeded' },
  { label: '已失败', value: 'failed' },
  { label: '已超时', value: 'timeout' },
  { label: '已取消', value: 'canceled' },
]
const STATUS_LABEL: Record<string, string> = { pending: '待执行', running: '执行中', succeeded: '已成功', failed: '已失败', timeout: '已超时', canceled: '已取消' }
const STATUS_TONE: Record<string, string> = { pending: 'neutral', running: 'info', succeeded: 'ok', failed: 'danger', timeout: 'danger', canceled: 'neutral' }

const list = useListPage<CommandRow, { status: string }>({
  query: { status: '' },
  fetch: params => fleetApi.commands.list(params),
})
const socket = useNodeSocket(() => undefined, { onCommand: () => list.load() })

const columns: Column[] = [
  { key: 'type', title: '命令', minWidth: 160 },
  { key: 'nodeName', title: '节点', minWidth: 150 },
  { key: 'status', title: '状态', width: 100 },
  { key: 'message', title: '说明', minWidth: 160 },
  { key: 'createdAt', title: '创建时间', width: 170 },
  { key: 'finishedAt', title: '结束时间', width: 170 },
  { key: 'actions', title: '操作', width: 130, className: 'col-actions' },
]

const detail = ref<CommandRow | null>(null)
const nodes = ref<NodeRow[]>([])
const nodeOptions = computed(() => nodes.value.map(node => ({ label: `${node.name} · ${node.status === 'online' ? '在线' : '离线'}`, value: node.id })))
const dialog = ref(false)
const saving = ref(false)
const form = reactive({ nodeId: null as number | null, type: 'reboot', payload: '{}', timeoutSeconds: 60 })
const error = ref('')
const TYPES = [
  { label: '重启', value: 'reboot' },
  { label: '采集诊断', value: 'diagnostics.collect' },
  { label: '刷新配置', value: 'config.refresh' },
  { label: '自定义', value: 'custom' },
]

async function openCreate() {
  form.nodeId = null
  form.type = 'reboot'
  form.payload = '{}'
  form.timeoutSeconds = 60
  error.value = ''
  dialog.value = true
  try {
    nodes.value = (await deviceApi.nodes.list({ pageNum: 1, pageSize: 100 })).rows
  } catch (failure) {
    toastError(failure, '节点加载失败')
  }
}

async function submit() {
  if (!form.nodeId || !form.type.trim()) {
    error.value = '请选择节点并填写命令类型'
    return
  }
  try {
    JSON.parse(form.payload || '{}')
  } catch {
    error.value = '载荷必须是合法 JSON'
    return
  }
  saving.value = true
  try {
    await fleetApi.commands.create({ nodeId: form.nodeId, type: form.type.trim(), payload: form.payload || '{}', timeoutSeconds: form.timeoutSeconds ?? 60 })
    pushToast('命令已创建；Agent 在线会立即收到，否则等待轮询')
    dialog.value = false
    await list.load()
  } catch (failure) {
    toastError(failure, '命令创建失败')
  } finally {
    saving.value = false
  }
}

async function cancel(row: CommandRow) {
  try {
    await fleetApi.commands.cancel(row.id)
    pushToast('命令已取消')
    await list.load()
  } catch (failure) {
    toastError(failure, '取消失败')
  }
}

function pretty(value: string) {
  if (!value) return '（无）'
  try {
    return JSON.stringify(JSON.parse(value), null, 2)
  } catch {
    return value
  }
}

onMounted(() => {
  list.load()
})
</script>

<template>
  <div :data-ws-state="socket.state">
    <ListPage title="命令中心" desc="向 Agent 下发命令并跟踪执行结果；在线通道直推，离线节点走轮询。" table-title="命令记录" :list="list" :columns="columns" :min-width="1050">
      <template #search><Select v-model="list.query.status" :options="STATUSES" placeholder="全部状态" /></template>
      <template #tools><PermissionButton perm="device:command:send" icon="command" label="下发命令" @click="openCreate" /></template>
      <template #cell-type="{ row }"><span class="mono">{{ row.type }}</span></template>
      <template #cell-status="{ row }"><Tag :tone="STATUS_TONE[row.status] || 'neutral'" dot>{{ STATUS_LABEL[row.status] || row.status }}</Tag></template>
      <template #cell-message="{ row }"><span class="faint">{{ row.message || '—' }}</span></template>
      <template #cell-createdAt="{ row }"><span class="mono soft">{{ row.createdAt }}</span></template>
      <template #cell-finishedAt="{ row }"><span class="mono soft">{{ row.finishedAt || '—' }}</span></template>
      <template #cell-actions="{ row }">
        <PermissionButton perm="device:command:list" variant="text" size="sm" icon="search" label="详情" @click="detail = row" />
        <PermissionButton v-if="row.status === 'pending' || row.status === 'running'" perm="device:command:send" variant="text" size="sm" danger icon="x" label="取消" @click="cancel(row)" />
      </template>
    </ListPage>

    <FormDialog :open="dialog" title="下发命令" sub="命令载荷是 JSON；超时时间单位秒。" :saving="saving" @close="dialog = false" @submit="submit">
      <Field label="目标节点" required><Select v-model="form.nodeId" :options="nodeOptions" placeholder="选择节点" filterable /></Field>
      <Field label="命令类型" required><Select v-model="form.type" :options="TYPES" filterable /></Field>
      <Field label="载荷（JSON）"><TextInput v-model="form.payload" placeholder="{}" /></Field>
      <Field label="超时秒数"><TextInput v-model.number="form.timeoutSeconds" placeholder="60" /></Field>
      <p v-if="error" class="form-error">{{ error }}</p>
    </FormDialog>

    <Drawer :open="Boolean(detail)" title="命令详情" :sub="detail ? `${detail.type} · ${detail.nodeName}` : ''" :width="560" @close="detail = null">
      <div v-if="detail" class="detail-stack">
        <div class="kv"><div class="kv-label">状态</div><div class="kv-value"><StatusTag :status="detail.status === 'succeeded'" on="已成功" off="未成功" /> {{ STATUS_LABEL[detail.status] }}</div></div>
        <div class="kv"><div class="kv-label">创建 / 结束</div><div class="kv-value mono">{{ detail.createdAt }} → {{ detail.finishedAt || '—' }}</div></div>
        <div class="kv"><div class="kv-label">载荷</div><pre class="code-block">{{ pretty(detail.payload) }}</pre></div>
        <div class="kv"><div class="kv-label">结果</div><pre class="code-block">{{ pretty(detail.result) }}</pre></div>
        <div class="kv"><div class="kv-label">说明</div><div class="kv-value">{{ detail.message || '—' }}</div></div>
      </div>
    </Drawer>
  </div>
</template>
