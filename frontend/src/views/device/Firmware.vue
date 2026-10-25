<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { Checkbox, DataTable, Drawer, Field, Select, Tag, TextInput, type Column } from '@/components/ui'
import { FirmwareRolloutDetail, FirmwareRolloutDialog, FormDialog, ListPage, PermissionButton } from '@/components/business'
import { deviceApi, fleetApi, systemApi, type FirmwarePolicy, type FirmwareRolloutAggregate, type FirmwareRow, type FirmwareTaskRow, type NodeRow } from '@/api'
import { currentUser } from '@/composables/auth'
import { useListPage } from '@/composables/list'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'
import { formatLocalDateTime } from '@/utils/time'

const columns: Column[] = [
  { key: 'name', title: '固件', minWidth: 200 },
  { key: 'version', title: '版本', width: 120 },
  { key: 'signed', title: '签名', width: 140 },
  { key: 'sizeBytes', title: '大小', width: 110, align: 'right' },
  { key: 'sha256', title: 'SHA-256', minWidth: 260 },
  { key: 'createdAt', title: '上传时间', width: 170 },
  { key: 'actions', title: '操作', width: 320, className: 'col-actions' },
]

const list = useListPage<FirmwareRow, { keyword: string }>({
  query: { keyword: '' },
  fetch: params => fleetApi.firmware.list(params),
})

function sizeText(bytes: number) {
  if (bytes >= 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MiB`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KiB`
  return `${bytes} B`
}

/* ── 上传 ── */
const uploadOpen = ref(false)
const uploadSaving = ref(false)
const uploadFile = ref<File | null>(null)
const uploadProgress = ref(0)
const platformAdmin = Boolean(currentUser.value?.platformAdmin)
const tenants = ref<Array<{ id: number; name: string; code: string }>>([])
const tenantOptions = ref<Array<{ label: string; value: number }>>([])
const policy = ref<FirmwarePolicy | null>(null)
const uploadForm = reactive({ name: '', version: '', tenantId: currentUser.value?.tenantId ?? null as number | null, signature: '', signingKeyId: '' })
const uploadError = ref('')

async function loadPolicy() {
  try {
    policy.value = await fleetApi.firmware.policy()
  } catch (error) {
    toastError(error, '固件签名策略加载失败')
  }
}

function openUpload() {
  uploadFile.value = null
  uploadProgress.value = 0
  uploadForm.name = ''
  uploadForm.version = ''
  uploadForm.signature = ''
  uploadForm.signingKeyId = ''
  uploadError.value = ''
  uploadOpen.value = true
}
function pickFile(event: Event) {
  const input = event.target as HTMLInputElement
  uploadFile.value = input.files?.[0] ?? null
  if (uploadFile.value && !uploadForm.name) uploadForm.name = uploadFile.value.name.replace(/\.[^.]+$/, '')
}
async function submitUpload() {
  if (!uploadFile.value || !uploadForm.name.trim() || !uploadForm.version.trim()) {
    uploadError.value = '请选择文件并填写名称与版本'
    return
  }
  const signature = uploadForm.signature.trim()
  const signingKeyId = uploadForm.signingKeyId.trim()
  // 服务端要求签名与密钥 ID 成对出现；两者缺一都会在上传开始前被拒绝
  if (Boolean(signature) !== Boolean(signingKeyId)) {
    uploadError.value = '签名与签名密钥 ID 必须同时填写或同时留空'
    return
  }
  if (policy.value?.requireSignature && !signature) {
    uploadError.value = '当前部署要求固件签名，请填写签名与密钥 ID'
    return
  }
  uploadSaving.value = true
  uploadError.value = ''
  try {
    await fleetApi.firmware.upload(uploadFile.value, uploadForm.name.trim(), uploadForm.version.trim(), {
      signature: signature || undefined,
      signingKeyId: signingKeyId || undefined,
      onProgress: percent => { uploadProgress.value = percent },
      tenantId: platformAdmin ? uploadForm.tenantId ?? undefined : undefined,
    })
    pushToast('固件已上传')
    uploadOpen.value = false
    await list.load()
  } catch (error) {
    uploadError.value = error instanceof Error ? error.message : '上传失败'
  } finally {
    uploadSaving.value = false
  }
}

/* ── 下发 ── */
const deployOpen = ref(false)
const deployTarget = ref<FirmwareRow | null>(null)
const deployNodeIds = ref<number[]>([])
const deployBatchSize = ref(1)
const rollbackOpen = ref(false)
const rollbackTarget = ref<FirmwareRow | null>(null)
const rollbackNodeIds = ref<number[]>([])
const deploySaving = ref(false)
const nodes = ref<NodeRow[]>([])
const nodeOptions = computed(() => nodes.value.map(node => ({ label: `${node.name} · ${node.status === 'online' ? '在线' : '离线'}`, value: node.id })))
async function openDeploy(row: FirmwareRow) {
  deployTarget.value = row
  deployNodeIds.value = []
  deployBatchSize.value = 1
  deployOpen.value = true
  try {
    nodes.value = (await deviceApi.nodes.list({ pageNum: 1, pageSize: 100 })).rows
  } catch (error) {
    toastError(error, '节点加载失败')
  }
}
function toggleDeployNode(id: number, checked: boolean) {
  deployNodeIds.value = checked ? [...deployNodeIds.value, id] : deployNodeIds.value.filter(nodeId => nodeId !== id)
}

async function submitDeploy() {
  if (!deployTarget.value || !deployNodeIds.value.length) return
  deploySaving.value = true
  try {
    const result = await fleetApi.firmware.deploy(deployTarget.value.id, deployNodeIds.value, deployBatchSize.value)
    if (Array.isArray(result)) {
      pushToast('固件下发任务已创建，Agent 会下载并回报状态')
      deployOpen.value = false
      return
    }
    if (result.deferredNodeIds.length) {
      // 保持对话框打开并选中剩余节点，便于按批继续灰度
      deployNodeIds.value = result.deferredNodeIds
      pushToast(`本批已下发 ${result.deployed.length} 个节点，剩余 ${result.deferredNodeIds.length} 个可继续下发`)
    } else {
      pushToast(`本批已下发 ${result.deployed.length} 个节点，灰度完成`)
      deployOpen.value = false
    }
  } catch (error) {
    toastError(error, '固件下发失败')
  } finally {
    deploySaving.value = false
  }
}

function openRollback(row: FirmwareRow) {
  rollbackTarget.value = row
  rollbackNodeIds.value = []
  rollbackOpen.value = true
  if (!nodes.value.length) {
    deviceApi.nodes.list({ pageNum: 1, pageSize: 100 }).then(page => { nodes.value = page.rows }).catch(() => undefined)
  }
}

async function submitRollback() {
  if (!rollbackTarget.value || !rollbackNodeIds.value.length) return
  deploySaving.value = true
  try {
    const result = await fleetApi.firmware.rollback(rollbackTarget.value.id, rollbackNodeIds.value)
    pushToast(`已为 ${result.length} 个节点安排回滚`)
    rollbackOpen.value = false
    await list.load()
  } catch (error) {
    toastError(error, '固件回滚失败')
  } finally {
    deploySaving.value = false
  }
}

/* ── 灰度发布 ── */
const rolloutOpen = ref(false)
const rolloutFirmware = ref<FirmwareRow | null>(null)
const rolloutNodesLoading = ref(false)
const rolloutDetailOpen = ref(false)
const rolloutDetailId = ref<number | null>(null)

async function loadNodes() {
  rolloutNodesLoading.value = true
  try {
    nodes.value = (await deviceApi.nodes.list({ pageNum: 1, pageSize: 100 })).rows
  } catch (error) {
    toastError(error, '节点加载失败')
  } finally {
    rolloutNodesLoading.value = false
  }
}

async function openRollout(row: FirmwareRow) {
  rolloutFirmware.value = row
  rolloutOpen.value = true
  if (!nodes.value.length) await loadNodes()
}

function openRolloutDetail(rollout: FirmwareRolloutAggregate) {
  rolloutOpen.value = false
  rolloutDetailId.value = rollout.id
  rolloutDetailOpen.value = true
}

/* ── 任务 ── */
const tasksOpen = ref(false)
const tasks = ref<FirmwareTaskRow[]>([])
const tasksLoading = ref(false)
const taskColumns: Column[] = [
  { key: 'nodeName', title: '节点', minWidth: 150 },
  { key: 'status', title: '状态', width: 120 },
  { key: 'progress', title: '进度', width: 160 },
  { key: 'message', title: '说明', minWidth: 200 },
  { key: 'updatedAt', title: '更新时间', width: 170 },
]
async function openTasks(row: FirmwareRow) {
  deployTarget.value = row
  tasksOpen.value = true
  tasksLoading.value = true
  try {
    tasks.value = await fleetApi.firmware.tasks(row.id)
  } catch (error) {
    toastError(error, '任务加载失败')
  } finally {
    tasksLoading.value = false
  }
}

async function remove(row: FirmwareRow) {
  const ok = await confirm({ title: '删除固件', message: `确认删除「${row.name} ${row.version}」及其文件吗？`, confirmText: '删除', danger: true })
  if (!ok) return
  try {
    await fleetApi.firmware.remove(row.id)
    pushToast('固件已删除')
    await list.load()
  } catch (error) {
    toastError(error, '固件删除失败')
  }
}

onMounted(async () => {
  list.load()
  await loadPolicy()
  if (!platformAdmin) return
  try {
    const page = await systemApi.tenants.list({ pageNum: 1, pageSize: 100, status: '1' })
    tenants.value = page.rows.map(row => ({ id: row.id, name: row.name, code: row.code }))
    tenantOptions.value = tenants.value.map(t => ({ label: `${t.name}（${t.code}）`, value: t.id }))
  } catch {
    tenantOptions.value = []
  }
})
</script>

<template>
  <div>
    <ListPage title="固件管理" desc="上传固件、查看摘要并按节点下发；Agent 通过 mTLS 下载支持 Range 与 ETag。" table-title="固件列表" :list="list" :columns="columns" :min-width="1220">
      <template #search><TextInput v-model="list.query.keyword" icon="search" placeholder="搜索名称或版本" clearable /></template>
      <template #tools>
        <Tag v-if="policy" :tone="policy.requireSignature ? 'danger' : 'neutral'" dot>
          {{ policy.requireSignature ? '强制固件签名' : '固件签名可选' }}
        </Tag>
        <PermissionButton perm="device:firmware:upload" icon="upload" label="上传固件" @click="openUpload" />
      </template>
      <template #cell-name="{ row }">
        <div class="cell-title">{{ row.name }}</div>
        <div class="cell-sub">{{ row.fileName }}</div>
      </template>
      <template #cell-signed="{ row }">
        <Tag :tone="row.signed ? 'ok' : (policy?.requireSignature ? 'danger' : 'neutral')" dot>{{ row.signed ? '已签名' : '未签名' }}</Tag>
        <div v-if="row.signed && row.signatureKeyId" class="cell-sub mono">{{ row.signatureKeyId }}</div>
        <div v-else-if="policy?.requireSignature" class="cell-sub">策略要求签名，不能下发</div>
      </template>
      <template #cell-sizeBytes="{ row }"><span class="tnum soft">{{ sizeText(row.sizeBytes) }}</span></template>
      <template #cell-sha256="{ row }"><span class="mono faint" style="font-size:11px">{{ row.sha256 }}</span></template>
      <template #cell-createdAt="{ row }"><span class="mono soft">{{ formatLocalDateTime(row.createdAt) }}</span></template>
      <template #cell-actions="{ row }">
        <PermissionButton perm="device:firmware:upload" variant="text" size="sm" icon="send" label="下发" :disabled="Boolean(policy?.requireSignature && !row.signed)" @click="openDeploy(row)" />
        <PermissionButton perm="device:firmware:upload" variant="text" size="sm" icon="layers" label="灰度发布" :disabled="Boolean(policy?.requireSignature && !row.signed)" @click="openRollout(row)" />
        <PermissionButton perm="device:firmware:upload" variant="text" size="sm" icon="refresh" label="回滚" :disabled="Boolean(policy?.requireSignature && !row.signed)" @click="openRollback(row)" />
        <PermissionButton perm="device:firmware:list" variant="text" size="sm" icon="activity" label="任务" @click="openTasks(row)" />
        <PermissionButton perm="device:firmware:remove" variant="text" size="sm" danger icon="trash" label="删除" @click="remove(row)" />
      </template>
    </ListPage>

    <FormDialog
      :open="uploadOpen"
      title="上传固件"
      :sub="policy?.requireSignature ? '当前部署强制校验固件签名；签名与密钥 ID 必填。' : '支持大文件流式上传；签名可选，填写时需与密钥 ID 成对。'"
      :saving="uploadSaving"
      :width="680"
      @close="uploadOpen = false"
      @submit="submitUpload"
    >
      <Field v-if="platformAdmin" label="所属租户" hint="默认落在当前租户">
        <Select v-model="uploadForm.tenantId" :options="tenantOptions" placeholder="选择租户" />
      </Field>
      <Field label="固件名称" required><TextInput v-model="uploadForm.name" placeholder="例如 EdgePulse OS" /></Field>
      <Field label="版本" required><TextInput v-model="uploadForm.version" placeholder="1.2.3" /></Field>
      <Field label="文件" required>
        <input type="file" @change="pickFile" />
        <div v-if="uploadFile" class="faint" style="margin-top:6px">{{ uploadFile.name }} · {{ sizeText(uploadFile.size) }}</div>
      </Field>
      <Field
        label="签名（Base64）"
        :required="policy?.requireSignature"
        :hint="policy?.trustedKeyIds.length ? `信任键：${policy.trustedKeyIds.join('、')}` : 'Ed25519 签名的 Base64 文本；部署配置信任键后才能校验'"
        span
      >
        <TextInput v-model="uploadForm.signature" multiline :rows="3" placeholder="粘贴 Base64 签名" />
      </Field>
      <Field label="签名密钥 ID" :required="policy?.requireSignature" hint="与签名同时提供" span>
        <TextInput v-model="uploadForm.signingKeyId" placeholder="例如 firmware-2026" />
      </Field>
      <div v-if="uploadSaving" class="faint" style="margin-top:6px">上传进度 {{ uploadProgress }}%</div>
      <p v-if="uploadError" class="form-error">{{ uploadError }}</p>
    </FormDialog>

    <FormDialog :open="deployOpen" title="灰度下发固件" :sub="deployTarget ? `${deployTarget.name} ${deployTarget.version}` : ''" :saving="deploySaving" @close="deployOpen = false" @submit="submitDeploy">
      <Field label="目标节点" required hint="按列表顺序取本批节点；剩余节点会保留在对话框中">
        <div class="node-picker">
          <label v-for="node in nodes" :key="node.id" class="node-picker-row">
            <Checkbox :model-value="deployNodeIds.includes(node.id)" @update:model-value="toggleDeployNode(node.id, $event)" />
            <span>{{ node.name }}</span>
            <span class="faint">{{ node.status === 'online' ? '在线' : '离线' }}</span>
          </label>
        </div>
      </Field>
      <Field label="本批数量" hint="1–100；小于所选节点数时剩余节点留在对话框中"><TextInput v-model.number="deployBatchSize" type="number" /></Field>
    </FormDialog>

    <FormDialog :open="rollbackOpen" title="回滚固件" :sub="rollbackTarget ? `从 ${rollbackTarget.name} ${rollbackTarget.version} 回滚` : ''" :saving="deploySaving" @close="rollbackOpen = false" @submit="submitRollback">
      <Field label="目标节点" required hint="只对记录过上一版本的节点生效">
        <div class="node-picker">
          <label v-for="node in nodes" :key="node.id" class="node-picker-row">
            <Checkbox :model-value="rollbackNodeIds.includes(node.id)" @update:model-value="checked => rollbackNodeIds = checked ? [...rollbackNodeIds, node.id] : rollbackNodeIds.filter(id => id !== node.id)" />
            <span>{{ node.name }}</span>
          </label>
        </div>
      </Field>
    </FormDialog>

    <FirmwareRolloutDialog
      :open="rolloutOpen"
      :firmware="rolloutFirmware"
      :nodes="nodes"
      :nodes-loading="rolloutNodesLoading"
      @close="rolloutOpen = false"
      @created="openRolloutDetail"
    />
    <FirmwareRolloutDetail :open="rolloutDetailOpen" :rollout-id="rolloutDetailId" @close="rolloutDetailOpen = false" />

    <Drawer :open="tasksOpen" title="下发任务" :sub="deployTarget ? `${deployTarget.name} ${deployTarget.version}` : ''" :width="820" @close="tasksOpen = false">
      <DataTable :columns="taskColumns" :data="tasks" :loading="tasksLoading" :min-width="760" empty-text="还没有下发任务">
        <template #cell-status="{ row }"><Tag :tone="row.status === 'installed' ? 'ok' : row.status === 'failed' ? 'danger' : 'info'">{{ row.status }}</Tag></template>
        <template #cell-progress="{ row }"><span class="tnum">{{ row.progress }}%</span></template>
        <template #cell-updatedAt="{ row }"><span class="mono soft">{{ formatLocalDateTime(row.updatedAt) }}</span></template>
      </DataTable>
    </Drawer>
  </div>
</template>

<style scoped>
.node-picker {
  display: grid;
  gap: 6px;
  max-height: 220px;
  overflow: auto;
  padding: 8px;
  border: 1px solid var(--line);
  border-radius: 8px;
}

.node-picker-row {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 32px;
}

.node-picker-row .faint {
  margin-left: auto;
  font-size: 12px;
}
</style>
