<script setup lang="ts">
import { reactive, ref, watch } from 'vue'
import { Button, DataTable, Field, Modal, Select, Tag, TextInput, type Column } from '@/components/ui'
import { deviceApi, type EnrollmentTokenCreated, type EnrollmentTokenRow } from '@/api'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'
import { copyText } from '@/utils/clipboard'

const props = defineProps<{ open: boolean }>()
const emit = defineEmits<{ (e: 'close'): void }>()

const ACTIONS = [
  { label: '注册新节点', value: 'enroll' },
  { label: '重绑已有节点', value: 'rebind' },
]
const STATUS_LABEL: Record<string, string> = { active: '有效', used: '已使用', expired: '已过期', revoked: '已吊销' }
const STATUS_TONE: Record<string, string> = { active: 'ok', used: 'info', expired: 'neutral', revoked: 'danger' }
const ACTION_LABEL: Record<string, string> = { enroll: '注册', rebind: '重绑' }

const columns: Column[] = [
  { key: 'nodeName', title: '节点名', minWidth: 140 },
  { key: 'action', title: '动作', width: 90 },
  { key: 'status', title: '状态', width: 100 },
  { key: 'expiresAt', title: '到期时间', width: 170 },
  { key: 'createdAt', title: '创建时间', width: 170 },
  { key: 'actions', title: '操作', width: 100, className: 'col-actions' },
]

const rows = ref<EnrollmentTokenRow[]>([])
const loading = ref(false)
const creating = ref(false)
const created = ref<EnrollmentTokenCreated | null>(null)
const form = reactive({ action: 'enroll' as string, nodeName: '', expiresInSeconds: 86400 as number | null })
const errors = ref<Partial<Record<'nodeName' | 'expiresInSeconds', string>>>({})

async function load() {
  loading.value = true
  try {
    rows.value = await deviceApi.enrollmentTokens.list()
  } catch (error) {
    toastError(error, '注册令牌加载失败')
  } finally {
    loading.value = false
  }
}

watch(() => props.open, (open) => {
  // 明文只在本次打开期间保留；关闭即丢弃，界面不再提供回看入口
  created.value = null
  if (!open) return
  errors.value = {}
  form.action = 'enroll'
  form.nodeName = ''
  form.expiresInSeconds = 86400
  void load()
}, { immediate: true })

async function create() {
  errors.value = {}
  const seconds = Number(form.expiresInSeconds)
  if (!Number.isInteger(seconds) || seconds < 300 || seconds > 2592000) {
    errors.value.expiresInSeconds = '有效期必须是 300 到 2592000 之间的整数秒'
  }
  if (Object.keys(errors.value).length) return
  creating.value = true
  try {
    created.value = await deviceApi.enrollmentTokens.create({
      action: form.action === 'rebind' ? 'rebind' : 'enroll',
      nodeName: form.nodeName.trim() || undefined,
      expiresInSeconds: seconds,
    })
    pushToast('注册令牌已生成')
    await load()
  } catch (error) {
    toastError(error, '注册令牌生成失败')
  } finally {
    creating.value = false
  }
}

async function copyToken() {
  if (!created.value) return
  const copied = await copyText(created.value.token)
  if (copied) pushToast('令牌已复制')
  else pushToast('复制失败，请手动选中令牌文本', 'danger')
}

async function revoke(row: EnrollmentTokenRow) {
  const ok = await confirm({
    title: '吊销注册令牌',
    message: `确认吊销${row.nodeName ? `「${row.nodeName}」的` : '这条'}注册令牌吗？吊销后无法再用它注册。`,
    confirmText: '吊销',
    danger: true,
  })
  if (!ok) return
  try {
    await deviceApi.enrollmentTokens.revoke(row.id)
    pushToast('注册令牌已吊销')
    await load()
  } catch (error) {
    toastError(error, '注册令牌吊销失败')
  }
}
</script>

<template>
  <Modal :open="open" title="注册令牌" sub="令牌只用于 Agent 首次注册或证书重绑；明文只显示一次。" :width="880" @close="emit('close')">
    <div class="token-once" v-if="created">
      <div>
        <div class="section-label">令牌只显示这一次</div>
        <p class="faint" style="margin: 4px 0 0">关闭对话框后无法再次查看，请立即复制并交给现场部署人员。</p>
      </div>
      <pre class="code-block token-value">{{ created.token }}</pre>
      <div class="token-actions">
        <Button variant="primary" size="sm" icon="copy" @click="copyToken">复制令牌</Button>
        <Tag tone="ok" dot>有效至 {{ created.expiresAt }}</Tag>
      </div>
    </div>

    <form class="form-grid token-form" @submit.prevent="create">
      <Field label="动作" required>
        <Select v-model="form.action" :options="ACTIONS" />
      </Field>
      <Field label="节点名" hint="留空表示任意节点可用；填写后只允许该节点消费">
        <TextInput v-model="form.nodeName" placeholder="例如 edge-sh-01" />
      </Field>
      <Field label="有效期（秒）" :error="errors.expiresInSeconds" hint="默认 86400 秒；允许范围 300–2592000">
        <TextInput v-model.number="form.expiresInSeconds" type="number" :invalid="!!errors.expiresInSeconds" />
      </Field>
      <div class="token-form-submit span-2">
        <Button type="submit" variant="primary" icon="key" :loading="creating">生成令牌</Button>
      </div>
    </form>

    <section style="margin-top: 18px">
      <div class="section-label">已签发令牌</div>
      <DataTable :columns="columns" :data="rows" :loading="loading" :min-width="800" empty-text="还没有注册令牌">
        <template #cell-nodeName="{ row }"><span :class="row.nodeName ? 'mono' : 'faint'">{{ row.nodeName || '任意节点' }}</span></template>
        <template #cell-action="{ row }"><Tag tone="info">{{ ACTION_LABEL[row.action] || row.action }}</Tag></template>
        <template #cell-status="{ row }"><Tag :tone="STATUS_TONE[row.status] || 'neutral'" dot>{{ STATUS_LABEL[row.status] || row.status }}</Tag></template>
        <template #cell-expiresAt="{ row }"><span class="mono soft">{{ row.expiresAt || '—' }}</span></template>
        <template #cell-createdAt="{ row }"><span class="mono soft">{{ row.createdAt || '—' }}</span></template>
        <template #cell-actions="{ row }">
          <Button v-if="row.status === 'active'" variant="text" size="sm" danger icon="ban" @click="revoke(row)">吊销</Button>
          <span v-else class="faint">—</span>
        </template>
      </DataTable>
    </section>

    <template #footer>
      <Button variant="ghost" @click="emit('close')">关闭</Button>
    </template>
  </Modal>
</template>
