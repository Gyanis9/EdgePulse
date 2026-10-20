<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { DataTable, Drawer, Field, Select, Tag, TextInput, type Column } from '@/components/ui'
import { FormDialog, ListPage, PermissionButton } from '@/components/business'
import { deviceApi, fleetApi, type FirmwareRow, type FirmwareTaskRow, type NodeRow } from '@/api'
import { useListPage } from '@/composables/list'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'

const columns: Column[] = [
  { key: 'name', title: '固件', minWidth: 200 },
  { key: 'version', title: '版本', width: 120 },
  { key: 'sizeBytes', title: '大小', width: 110, align: 'right' },
  { key: 'sha256', title: 'SHA-256', minWidth: 260 },
  { key: 'createdAt', title: '上传时间', width: 170 },
  { key: 'actions', title: '操作', width: 220, className: 'col-actions' },
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
const uploadForm = reactive({ name: '', version: '' })
const uploadError = ref('')

function openUpload() {
  uploadFile.value = null
  uploadProgress.value = 0
  uploadForm.name = ''
  uploadForm.version = ''
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
  uploadSaving.value = true
  uploadError.value = ''
  try {
    await fleetApi.firmware.upload(uploadFile.value, uploadForm.name.trim(), uploadForm.version.trim(), percent => { uploadProgress.value = percent })
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
const deployNodeId = ref<number | null>(null)
const deploySaving = ref(false)
const nodes = ref<NodeRow[]>([])
const nodeOptions = computed(() => nodes.value.map(node => ({ label: `${node.name} · ${node.status === 'online' ? '在线' : '离线'}`, value: node.id })))
async function openDeploy(row: FirmwareRow) {
  deployTarget.value = row
  deployNodeId.value = null
  deployOpen.value = true
  try {
    nodes.value = (await deviceApi.nodes.list({ pageNum: 1, pageSize: 100 })).rows
  } catch (error) {
    toastError(error, '节点加载失败')
  }
}
async function submitDeploy() {
  if (!deployTarget.value || !deployNodeId.value) return
  deploySaving.value = true
  try {
    await fleetApi.firmware.deploy(deployTarget.value.id, [deployNodeId.value])
    pushToast('固件下发任务已创建，Agent 会下载并回报状态')
    deployOpen.value = false
  } catch (error) {
    toastError(error, '固件下发失败')
  } finally {
    deploySaving.value = false
  }
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

onMounted(() => {
  list.load()
})
</script>

<template>
  <div>
    <ListPage title="固件管理" desc="上传固件、查看摘要并按节点下发；Agent 通过 mTLS 下载支持 Range 与 ETag。" table-title="固件列表" :list="list" :columns="columns" :min-width="1080">
      <template #search><TextInput v-model="list.query.keyword" icon="search" placeholder="搜索名称或版本" clearable /></template>
      <template #tools><PermissionButton perm="device:firmware:upload" icon="upload" label="上传固件" @click="openUpload" /></template>
      <template #cell-name="{ row }">
        <div class="cell-title">{{ row.name }}</div>
        <div class="cell-sub">{{ row.fileName }}</div>
      </template>
      <template #cell-sizeBytes="{ row }"><span class="tnum soft">{{ sizeText(row.sizeBytes) }}</span></template>
      <template #cell-sha256="{ row }"><span class="mono faint" style="font-size:11px">{{ row.sha256 }}</span></template>
      <template #cell-createdAt="{ row }"><span class="mono soft">{{ row.createdAt }}</span></template>
      <template #cell-actions="{ row }">
        <PermissionButton perm="device:firmware:upload" variant="text" size="sm" icon="send" label="下发" @click="openDeploy(row)" />
        <PermissionButton perm="device:firmware:list" variant="text" size="sm" icon="activity" label="任务" @click="openTasks(row)" />
        <PermissionButton perm="device:firmware:remove" variant="text" size="sm" danger icon="trash" label="删除" @click="remove(row)" />
      </template>
    </ListPage>

    <FormDialog :open="uploadOpen" title="上传固件" sub="支持大文件流式上传；摘要由服务端边收边算。" :saving="uploadSaving" @close="uploadOpen = false" @submit="submitUpload">
      <Field label="固件名称" required><TextInput v-model="uploadForm.name" placeholder="例如 EdgePulse OS" /></Field>
      <Field label="版本" required><TextInput v-model="uploadForm.version" placeholder="1.2.3" /></Field>
      <Field label="文件" required>
        <input type="file" @change="pickFile" />
        <div v-if="uploadFile" class="faint" style="margin-top:6px">{{ uploadFile.name }} · {{ sizeText(uploadFile.size) }}</div>
      </Field>
      <div v-if="uploadSaving" class="faint" style="margin-top:6px">上传进度 {{ uploadProgress }}%</div>
      <p v-if="uploadError" class="form-error">{{ uploadError }}</p>
    </FormDialog>

    <FormDialog :open="deployOpen" title="下发固件" :sub="deployTarget ? `${deployTarget.name} ${deployTarget.version}` : ''" :saving="deploySaving" @close="deployOpen = false" @submit="submitDeploy">
      <Field label="目标节点" required><Select v-model="deployNodeId" :options="nodeOptions" placeholder="选择节点" filterable /></Field>
    </FormDialog>

    <Drawer :open="tasksOpen" title="下发任务" :sub="deployTarget ? `${deployTarget.name} ${deployTarget.version}` : ''" :width="820" @close="tasksOpen = false">
      <DataTable :columns="taskColumns" :data="tasks" :loading="tasksLoading" :min-width="760" empty-text="还没有下发任务">
        <template #cell-status="{ row }"><Tag :tone="row.status === 'installed' ? 'ok' : row.status === 'failed' ? 'danger' : 'info'">{{ row.status }}</Tag></template>
        <template #cell-progress="{ row }"><span class="tnum">{{ row.progress }}%</span></template>
      </DataTable>
    </Drawer>
  </div>
</template>
