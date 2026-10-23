<script setup lang="ts">
import { onMounted, reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import { EnrollmentTokenDialog, ListPage, FormDialog, PermissionButton } from '@/components/business'
import { Field, Select, StatusTag, TextInput, type Column } from '@/components/ui'
import { deviceApi, fleetApi, systemApi, type NodeGroupRow, type NodeRow } from '@/api'
import { currentUser } from '@/composables/auth'
import { useListPage } from '@/composables/list'
import { useNodeSocket } from '@/composables/nodeSocket'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'

const columns: Column[] = [
  { key: 'name', title: '节点名', minWidth: 180 },
  { key: 'groupName', title: '分组', width: 140 },
  { key: 'status', title: '状态', width: 100 },
  { key: 'agentVersion', title: 'Agent 版本', width: 130 },
  { key: 'lastSeen', title: '最后心跳', minWidth: 190 },
  { key: 'labels', title: '标签', minWidth: 200 },
  { key: 'actions', title: '操作', width: 150, className: 'col-actions' },
]

const list = useListPage<NodeRow, { keyword: string; groupId: number | null }>({
  query: { keyword: '', groupId: null },
  fetch: params => deviceApi.nodes.list(params),
})
const router = useRouter()
const tokenDialog = ref(false)

function openDetail(node: NodeRow) {
  router.push(`/device/nodes/${node.id}`)
}

const platformAdmin = Boolean(currentUser.value?.platformAdmin)
const tenants = ref<Array<{ id: number; name: string; code: string }>>([])
const tenantOptions = ref<Array<{ label: string; value: number }>>([])
const groupRows = ref<NodeGroupRow[]>([])
const groupOptions = ref<Array<{ label: string; value: number; depth?: number }>>([])
function flattenGroups(nodes: NodeGroupRow[], depth = 0, output: Array<{ label: string; value: number; depth: number }> = []) {
  nodes.forEach(node => {
    output.push({ label: node.name, value: node.id, depth })
    flattenGroups(node.children ?? [], depth + 1, output)
  })
  return output
}
onMounted(async () => {
  try {
    groupRows.value = await fleetApi.nodeGroups.tree()
    groupOptions.value = [{ label: '全部分组', value: 0 }, ...flattenGroups(groupRows.value)]
  } catch (error) {
    toastError(error, '分组加载失败')
  }
  if (platformAdmin) {
    try {
      const page = await systemApi.tenants.list({ pageNum: 1, pageSize: 100, status: '1' })
      tenants.value = page.rows.map(row => ({ id: row.id, name: row.name, code: row.code }))
      tenantOptions.value = tenants.value.map(t => ({ label: `${t.name}（${t.code}）`, value: t.id }))
    } catch {
      tenantOptions.value = []
    }
  }
})

const socket = useNodeSocket(node => {
  const row = list.rows.value.find(candidate => candidate.id === node.id)
  // 已知行就地合并版本与状态；新节点不在当前页时重新拉取，保持分页与总数一致
  if (row) Object.assign(row, node)
  else list.load()
})
const socketState = socket.state

function labelText(node: NodeRow) {
  if (!node.labels || !Object.keys(node.labels).length) return '—'
  return Object.entries(node.labels).map(([key, value]) => `${key}=${value}`).join('，')
}

const dialog = ref(false)
const saving = ref(false)
const form = reactive({ name: '', labels: '', groupId: 200, tenantId: currentUser.value?.tenantId ?? null as number | null })
const errors = ref<Partial<Record<'name' | 'labels', string>>>({})

function openCreate() {
  form.name = ''
  form.labels = ''
  form.groupId = groupOptions.value.find(option => option.value !== 0)?.value ?? 200
  form.tenantId = currentUser.value?.tenantId ?? null
  errors.value = {}
  dialog.value = true
}

async function submit() {
  errors.value = {}
  if (!form.name.trim()) errors.value.name = '请输入节点名'
  let labels: Record<string, string> | null = null
  if (form.labels.trim()) {
    try {
      const parsed: unknown = JSON.parse(form.labels)
      if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) throw new Error('not-object')
      labels = parsed as Record<string, string>
    } catch {
      errors.value.labels = '标签必须是 JSON 对象，例如 {"site":"shanghai"}'
    }
  }
  if (Object.keys(errors.value).length) return
  saving.value = true
  try {
    await deviceApi.nodes.create({ name: form.name.trim(), labels, groupId: form.groupId, tenantId: platformAdmin ? form.tenantId ?? undefined : undefined })
    pushToast('节点已创建')
    dialog.value = false
    await list.load()
  } catch (error) {
    toastError(error, '节点创建失败')
  } finally {
    saving.value = false
  }
}

async function remove(node: NodeRow) {
  const ok = await confirm({
    title: '删除节点',
    message: `确认删除节点「${node.name}」吗？该节点后续的心跳将无法再关联到记录。`,
    confirmText: '删除',
    danger: true,
  })
  if (!ok) return
  try {
    await deviceApi.nodes.remove(node.id)
    pushToast('节点已删除')
    await list.load()
  } catch (error) {
    toastError(error, '节点删除失败')
  }
}
</script>

<template>
  <div :data-ws-state="socketState">
    <ListPage title="节点" desc="边缘节点的注册、心跳与在线状态；写操作需要节点写权限。" table-title="节点列表" :list="list" :columns="columns" :min-width="980">
      <template #search>
        <TextInput v-model="list.query.keyword" icon="search" placeholder="搜索节点名" clearable />
        <Select v-model="list.query.groupId" :options="groupOptions" placeholder="全部分组" />
      </template>
      <template #tools>
        <PermissionButton perm="device:node:write" icon="key" label="注册令牌" @click="tokenDialog = true" />
        <PermissionButton perm="device:node:write" icon="plus" label="新增节点" @click="openCreate" />
      </template>
      <template #cell-status="{ row }">
        <StatusTag :status="row.status === 'online'" on="在线" off="离线" />
      </template>
      <template #cell-groupName="{ row }">
        <span class="faint">{{ row.groupName || '—' }}</span>
      </template>
      <template #cell-lastSeen="{ row }">
        <span class="mono faint">{{ row.lastSeen || '—' }}</span>
      </template>
      <template #cell-labels="{ row }">
        <span class="faint">{{ labelText(row) }}</span>
      </template>
      <template #cell-actions="{ row }">
        <div class="row-actions">
          <PermissionButton perm="device:node:list" variant="text" size="sm" icon="search" label="详情" @click="openDetail(row)" />
          <PermissionButton perm="device:node:write" variant="text" size="sm" danger icon="trash" label="删除" @click="remove(row)" />
        </div>
      </template>
    </ListPage>

    <EnrollmentTokenDialog :open="tokenDialog" @close="tokenDialog = false" />

    <FormDialog :open="dialog" title="新增节点" sub="名称唯一；标签是可选的 JSON 对象。" :saving="saving" @close="dialog = false" @submit="submit">
      <Field v-if="platformAdmin" label="所属租户" hint="默认落在当前租户">
        <Select v-model="form.tenantId" :options="tenantOptions" placeholder="选择租户" />
      </Field>
      <Field label="节点名" :error="errors.name" required>
        <TextInput v-model="form.name" placeholder="例如 edge-sh-01" :invalid="!!errors.name" />
      </Field>
      <Field label="所属分组" required>
        <Select v-model="form.groupId" :options="groupOptions.filter(option => option.value !== 0)" placeholder="选择分组" />
      </Field>
      <Field label="标签（JSON 对象）" :error="errors.labels" hint="留空表示无标签">
        <TextInput v-model="form.labels" placeholder='{"site":"shanghai"}' :invalid="!!errors.labels" />
      </Field>
    </FormDialog>
  </div>
</template>
