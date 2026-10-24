<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { DataTable, Drawer, Field, Select, Switch, Tag, TextInput, type Column } from '@/components/ui'
import { FormDialog, ListPage, PermissionButton } from '@/components/business'
import { systemApi, type NotificationChannelRow, type NotificationDeliveryRow } from '@/api'
import { currentUser } from '@/composables/auth'
import { useListPage } from '@/composables/list'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'
import { formatLocalDateTime } from '@/utils/time'
import { required, validate } from '@/utils/validate'

const list = useListPage<NotificationChannelRow, { keyword: string }>({
  query: { keyword: '' },
  fetch: params => systemApi.notifications.list(params) as Promise<any>,
})

const platformAdmin = computed(() => Boolean(currentUser.value?.platformAdmin))
const deliveriesOpen = ref(false)
const deliveriesLoading = ref(false)
const deliveries = ref<NotificationDeliveryRow[]>([])
const deliveryColumns: Column[] = [
  { key: 'createdAt', title: '时间', width: 170 },
  { key: 'channelName', title: '渠道', minWidth: 140 },
  { key: 'alarmLevel', title: '级别', width: 90 },
  { key: 'target', title: '目标', minWidth: 180 },
  { key: 'status', title: '结果', width: 90 },
  { key: 'attempts', title: '尝试', width: 70 },
  { key: 'error', title: '失败原因', minWidth: 200 },
]

async function openDeliveries() {
  deliveriesOpen.value = true
  deliveriesLoading.value = true
  try {
    const page = await systemApi.notifications.deliveries({ pageNum: 1, pageSize: 100 })
    deliveries.value = page.rows
  } catch (error) {
    toastError(error, '投递记录加载失败')
  } finally {
    deliveriesLoading.value = false
  }
}
const tenants = ref<Array<{ id: number; name: string; code: string }>>([])
const tenantOptions = computed(() => tenants.value.map(t => ({ label: `${t.name}（${t.code}）`, value: t.id })))

const columns = computed<Column[]>(() => [
  { key: 'name', title: '渠道', minWidth: 160 },
  { key: 'type', title: '类型', width: 100 },
  { key: 'endpoint', title: '目标地址', minWidth: 220 },
  { key: 'minLevel', title: '最低级别', width: 100 },
  { key: 'enabled', title: '状态', width: 90 },
  { key: 'actions', title: '操作', width: 120, className: 'col-actions' },
])

const LEVELS = [
  { label: '全部', value: 'info' },
  { label: '警告及以上', value: 'warning' },
  { label: '严重', value: 'critical' },
]
const TYPES = [
  { label: 'Webhook', value: 'webhook' },
  { label: '邮件', value: 'email' },
]

interface ChannelForm {
  id?: number
  tenantId: number | null
  name: string
  type: 'webhook' | 'email'
  endpoint: string
  recipients: string
  username: string
  sender: string
  caFile: string
  minLevel: string
  enabled: boolean
  secret: string
}
const emptyForm = (): ChannelForm => ({
  tenantId: currentUser.value?.tenantId ?? null,
  name: '', type: 'webhook', endpoint: '', recipients: '', username: '', sender: '', caFile: '', minLevel: 'warning', enabled: true, secret: '',
})
const dialog = ref(false)
const saving = ref(false)
const form = reactive<ChannelForm>(emptyForm())
const errors = ref<Partial<Record<keyof ChannelForm, string>>>({})
const isEdit = computed(() => Boolean(form.id))

onMounted(async () => {
  if (!platformAdmin.value) return
  try {
    const page = await systemApi.tenants.list({ pageNum: 1, pageSize: 100, status: '1' })
    tenants.value = page.rows.map(row => ({ id: row.id, name: row.name, code: row.code }))
  } catch {
    tenants.value = []
  }
})

function openCreate() {
  Object.assign(form, emptyForm(), { id: undefined })
  errors.value = {}
  dialog.value = true
}

function openEdit(row: NotificationChannelRow) {
  Object.assign(form, {
    id: row.id, tenantId: row.tenantId, name: row.name, type: row.type, endpoint: row.endpoint, recipients: row.recipients,
    username: row.username, sender: row.sender, caFile: row.caFile, minLevel: row.minLevel, enabled: row.enabled, secret: '',
  })
  errors.value = {}
  dialog.value = true
}

async function submit() {
  errors.value = validate(form, {
    name: [required('请输入渠道名称')],
    endpoint: [required(form.type === 'webhook' ? '请输入 Webhook 地址' : '请输入 SMTP 地址')],
    recipients: form.type === 'email' ? [required('请输入收件人')] : [],
  })
  if (Object.keys(errors.value).length) return
  saving.value = true
  const payload = {
    name: form.name.trim(),
    type: form.type,
    endpoint: form.endpoint.trim(),
    recipients: form.recipients,
    username: form.username,
    sender: form.sender,
    caFile: form.caFile,
    minLevel: form.minLevel,
    enabled: form.enabled,
    secret: form.secret || undefined,
    tenantId: platformAdmin.value && !form.id ? form.tenantId ?? undefined : undefined,
  }
  try {
    if (form.id) {
      await systemApi.notifications.update(form.id, payload)
      pushToast('通知渠道已更新')
    } else {
      await systemApi.notifications.create(payload)
      pushToast('通知渠道已新增')
    }
    dialog.value = false
    await list.load()
  } catch (error) {
    toastError(error, '通知渠道保存失败')
  } finally {
    saving.value = false
  }
}

async function remove(row: NotificationChannelRow) {
  const ok = await confirm({ title: '删除通知渠道', message: `确认删除「${row.name}」吗？后续告警将不再推送。`, confirmText: '删除', danger: true })
  if (!ok) return
  try {
    await systemApi.notifications.remove(row.id)
    pushToast('通知渠道已删除')
    await list.load()
  } catch (error) {
    toastError(error, '通知渠道删除失败')
  }
}
</script>

<template>
  <ListPage title="通知渠道" desc="把告警推送到 Webhook 或邮件；密钥只写入不回显。" table-title="渠道列表" :list="list" :columns="columns" :min-width="900">
    <template #actions>
      <PermissionButton perm="system:notification:list" variant="secondary" icon="activity" label="投递记录" @click="openDeliveries" />
      <PermissionButton perm="system:notification:add" variant="primary" icon="plus" @click="openCreate">新增渠道</PermissionButton>
    </template>

    <template #search>
      <Field label="关键词" wide>
        <TextInput v-model="list.query.keyword" icon="search" placeholder="渠道名称 / 地址 / 类型" clearable @enter="list.search()" />
      </Field>
    </template>

    <template #cell-name="{ row }"><span class="cell-title">{{ row.name }}</span></template>
    <template #cell-type="{ row }"><Tag :tone="row.type === 'webhook' ? 'info' : 'success'">{{ row.type === 'webhook' ? 'Webhook' : '邮件' }}</Tag></template>
    <template #cell-endpoint="{ row }"><span class="mono ellipsis">{{ row.endpoint }}</span></template>
    <template #cell-minLevel="{ row }">
      <Tag :tone="row.minLevel === 'critical' ? 'danger' : row.minLevel === 'warning' ? 'warning' : 'muted'">{{ LEVELS.find(l => l.value === row.minLevel)?.label ?? row.minLevel }}</Tag>
    </template>
    <template #cell-enabled="{ row }"><Tag :tone="row.enabled ? 'success' : 'muted'">{{ row.enabled ? '启用' : '停用' }}</Tag></template>
    <template #cell-actions="{ row }">
      <div class="row-actions">
        <PermissionButton perm="system:notification:edit" icon-only icon="edit" label="编辑" @click="openEdit(row)" />
        <PermissionButton perm="system:notification:remove" icon-only icon="trash" label="删除" danger @click="remove(row)" />
      </div>
    </template>

    <FormDialog :open="dialog" :title="isEdit ? '编辑通知渠道' : '新增通知渠道'" :saving="saving" @close="dialog = false" @submit="submit">
      <Field v-if="platformAdmin && !isEdit" label="所属租户">
        <Select v-model="form.tenantId" :options="tenantOptions" />
      </Field>
      <Field label="渠道名称" required :error="errors.name">
        <TextInput v-model="form.name" placeholder="如：值班群" :invalid="!!errors.name" />
      </Field>
      <Field label="类型">
        <Select v-model="form.type" :options="TYPES" :disabled="isEdit" />
      </Field>
      <Field label="最低级别">
        <Select v-model="form.minLevel" :options="LEVELS" />
      </Field>
      <Field :label="form.type === 'webhook' ? 'Webhook 地址' : 'SMTP 地址'" required :error="errors.endpoint"
             :hint="form.type === 'webhook' ? '接收 JSON POST 的 http(s) 地址' : 'smtp://host:25、smtps://host:465 或 starttls://host:587'" span>
        <TextInput v-model="form.endpoint" :placeholder="form.type === 'webhook' ? 'https://example.com/hook' : 'smtps://smtp.example.com:465'" :invalid="!!errors.endpoint" />
      </Field>
      <template v-if="form.type === 'email'">
        <Field label="收件人" required :error="errors.recipients" hint="多个地址用逗号分隔" span>
          <TextInput v-model="form.recipients" placeholder="ops@example.com" :invalid="!!errors.recipients" />
        </Field>
        <Field label="认证用户名">
          <TextInput v-model="form.username" placeholder="SMTP 用户名" autocomplete="off" />
        </Field>
        <Field label="发件人">
          <TextInput v-model="form.sender" placeholder="edgepulse@example.com" />
        </Field>
        <Field label="CA 文件" hint="私有 SMTP CA 的 PEM 路径；留空使用系统信任库" span>
          <TextInput v-model="form.caFile" placeholder="/etc/ssl/certs/internal-ca.pem" />
        </Field>
      </template>
      <Field :label="form.type === 'webhook' ? '签名密钥' : 'SMTP 口令'" :hint="isEdit ? '留空保持不变' : 'Webhook 用 HMAC-SHA256 签名；邮件为认证口令'" span>
        <TextInput v-model="form.secret" type="password" placeholder="留空保持不变" autocomplete="new-password" />
      </Field>
      <Field label="启用状态" group>
        <Switch :model-value="form.enabled" :label="form.enabled ? '启用，告警会推送' : '停用，不推送'" @update:model-value="form.enabled = $event" />
      </Field>
    </FormDialog>

    <Drawer :open="deliveriesOpen" title="投递记录" sub="最近 100 条 Webhook / 邮件投递结果" :width="760" @close="deliveriesOpen = false">
      <DataTable :columns="deliveryColumns" :data="deliveries" :loading="deliveriesLoading" :min-width="760" empty-text="还没有投递记录">
        <template #cell-status="{ row }"><Tag :tone="row.status === 'succeeded' ? 'success' : 'danger'">{{ row.status === 'succeeded' ? '成功' : '失败' }}</Tag></template>
        <template #cell-attempts="{ row }"><span class="tnum">{{ row.attempts }}</span></template>
        <template #cell-createdAt="{ row }"><span class="mono soft">{{ formatLocalDateTime(row.createdAt) }}</span></template>
      </DataTable>
    </Drawer>
  </ListPage>
</template>
