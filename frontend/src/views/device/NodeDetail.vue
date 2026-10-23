<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Button, DataTable, EmptyState, PageHeader, Panel, Segmented, StatusTag, Tag, type Column } from '@/components/ui'
import { NodeConfigPanel, PermissionButton } from '@/components/business'
import { ApiError, deviceApi, fleetApi, type AlarmRow, type CommandRow, type NodeDetailRow, type TelemetryPoint } from '@/api'
import { pathOf } from '@/composables/auth'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'
import { copyText } from '@/utils/clipboard'

type Tab = 'overview' | 'telemetry' | 'alarms' | 'commands' | 'firmware' | 'config'
const TABS: Array<{ label: string; value: Tab }> = [
  { label: '概览', value: 'overview' },
  { label: '遥测', value: 'telemetry' },
  { label: '告警', value: 'alarms' },
  { label: '命令', value: 'commands' },
  { label: '固件', value: 'firmware' },
  { label: '配置', value: 'config' },
]

const LEVEL_LABEL: Record<string, string> = { critical: '严重', warning: '警告', info: '提示' }
const LEVEL_TONE: Record<string, string> = { critical: 'danger', warning: 'neutral', info: 'info' }
const ALARM_STATUS_LABEL: Record<string, string> = { open: '未恢复', acknowledged: '已确认', resolved: '已恢复' }
const ALARM_STATUS_TONE: Record<string, string> = { open: 'danger', acknowledged: 'info', resolved: 'ok' }
const COMMAND_STATUS_LABEL: Record<string, string> = {
  pending: '待执行',
  running: '执行中',
  succeeded: '已成功',
  failed: '已失败',
  timeout: '已超时',
  canceled: '已取消',
}
const COMMAND_STATUS_TONE: Record<string, string> = {
  pending: 'neutral',
  running: 'info',
  succeeded: 'ok',
  failed: 'danger',
  timeout: 'danger',
  canceled: 'neutral',
}

const telemetryColumns: Column[] = [
  { key: 'metric', title: '指标', minWidth: 180 },
  { key: 'value', title: '数值', width: 120, align: 'right' },
  { key: 'unit', title: '单位', width: 90 },
  { key: 'collectedAt', title: '采集时间', width: 180 },
]
const alarmColumns: Column[] = [
  { key: 'level', title: '级别', width: 90 },
  { key: 'metric', title: '指标', minWidth: 140 },
  { key: 'message', title: '说明', minWidth: 180 },
  { key: 'status', title: '状态', width: 100 },
  { key: 'triggeredAt', title: '触发时间', width: 170 },
  { key: 'actions', title: '操作', width: 150, className: 'col-actions' },
]
const commandColumns: Column[] = [
  { key: 'type', title: '类型', minWidth: 160 },
  { key: 'status', title: '状态', width: 100 },
  { key: 'message', title: '说明', minWidth: 180 },
  { key: 'createdAt', title: '创建时间', width: 170 },
  { key: 'finishedAt', title: '结束时间', width: 170 },
]

const route = useRoute()
const router = useRouter()
const nodeId = Number(route.params.id)
const node = ref<NodeDetailRow | null>(null)
const tab = ref<Tab>('overview')
const latest = ref<TelemetryPoint[]>([])
const alarms = ref<AlarmRow[]>([])
const commands = ref<CommandRow[]>([])
const loading = ref(false)
const telemetryError = ref('')
const alarmsError = ref('')
const commandsError = ref('')

const fingerprintText = computed(() => {
  const value = node.value?.certFingerprint ?? ''
  if (!value) return '未绑定'
  return value.length > 40 ? `${value.slice(0, 20)}…${value.slice(-20)}` : value
})
const labelsText = computed(() => {
  const labels = node.value?.labels
  if (!labels || !Object.keys(labels).length) return '—'
  return Object.entries(labels).map(([key, value]) => `${key}=${value}`).join('，')
})
const canRevoke = computed(() => Boolean(node.value?.certFingerprint) && !node.value?.certRevokedAt)
const firmwareCommands = computed(() => commands.value.filter(row => row.type.startsWith('firmware.')))
const headerDesc = computed(() => node.value
  ? `最后心跳 ${node.value.lastSeen || '—'} · Agent ${node.value.agentVersion || '—'} · 分组 ${node.value.groupName || '未分组'}`
  : '查看证书、遥测、告警、命令、固件与配置')
const telemetryPath = computed(() => pathOf('device/Telemetry'))
const commandsPath = computed(() => pathOf('device/Commands'))
const firmwarePath = computed(() => pathOf('device/Firmware'))

function reasonOf(error: unknown, fallback: string) {
  return error instanceof ApiError ? error.message : fallback
}

async function load() {
  if (!Number.isInteger(nodeId) || nodeId <= 0) {
    pushToast('节点标识不合法', 'danger')
    return
  }
  loading.value = true
  telemetryError.value = ''
  alarmsError.value = ''
  commandsError.value = ''
  try {
    node.value = await deviceApi.nodes.get(nodeId)
  } catch (error) {
    toastError(error, '节点详情加载失败')
    loading.value = false
    return
  }
  const [latestResult, alarmResult, commandResult] = await Promise.allSettled([
    fleetApi.telemetry.latest(nodeId),
    fleetApi.alarms.list({ pageNum: 1, pageSize: 20, nodeId }),
    fleetApi.commands.list({ pageNum: 1, pageSize: 20, nodeId }),
  ])
  if (latestResult.status === 'fulfilled') latest.value = latestResult.value
  else {
    latest.value = []
    telemetryError.value = reasonOf(latestResult.reason, '最新遥测加载失败')
  }
  if (alarmResult.status === 'fulfilled') alarms.value = alarmResult.value.rows
  else {
    alarms.value = []
    alarmsError.value = reasonOf(alarmResult.reason, '节点告警加载失败')
  }
  if (commandResult.status === 'fulfilled') commands.value = commandResult.value.rows
  else {
    commands.value = []
    commandsError.value = reasonOf(commandResult.reason, '节点命令加载失败')
  }
  loading.value = false
}

async function loadAlarms() {
  try {
    alarms.value = (await fleetApi.alarms.list({ pageNum: 1, pageSize: 20, nodeId })).rows
    alarmsError.value = ''
  } catch (error) {
    alarmsError.value = reasonOf(error, '节点告警加载失败')
    toastError(error, '节点告警加载失败')
  }
}

async function ackAlarm(row: AlarmRow) {
  try {
    await fleetApi.alarms.ack(row.id)
    pushToast('告警已确认')
    await loadAlarms()
  } catch (error) {
    toastError(error, '确认失败')
  }
}

async function resolveAlarm(row: AlarmRow) {
  const ok = await confirm({ title: '恢复告警', message: `确认把「${row.metric}」标记为已恢复吗？`, confirmText: '恢复' })
  if (!ok) return
  try {
    await fleetApi.alarms.resolve(row.id)
    pushToast('告警已恢复')
    await loadAlarms()
  } catch (error) {
    toastError(error, '恢复失败')
  }
}

async function copyFingerprint() {
  if (!node.value?.certFingerprint) return
  if (await copyText(node.value.certFingerprint)) pushToast('证书指纹已复制')
  else pushToast('复制失败，请手动选择指纹文本', 'danger')
}

async function revokeCertificate() {
  if (!node.value) return
  const ok = await confirm({
    title: '吊销节点证书',
    message: `确认吊销「${node.value.name}」的证书吗？在线连接会立即断开，节点需要凭新注册令牌重新注册。`,
    confirmText: '吊销',
    danger: true,
  })
  if (!ok) return
  try {
    await deviceApi.nodes.revokeCertificate(nodeId)
    pushToast('节点证书已吊销')
    await load()
  } catch (error) {
    toastError(error, '证书吊销失败')
  }
}

function backToList() {
  router.push(pathOf('device/Nodes') ?? '/device/nodes')
}

function openNodePage(path: string | null) {
  if (!path) return
  router.push({ path, query: { nodeId: String(nodeId) } })
}

function openPage(path: string | null) {
  if (path) router.push(path)
}

onMounted(load)
</script>

<template>
  <div class="page">
    <PageHeader :title="node?.name || '节点详情'" :desc="headerDesc">
      <Button icon="refresh" :loading="loading" @click="load">刷新</Button>
      <Button icon="chevronLeft" @click="backToList">返回列表</Button>
    </PageHeader>

    <div class="node-detail-tabs">
      <Segmented v-model="tab" label="节点详情分区" :options="TABS" />
      <div v-if="node" class="node-detail-badges">
        <StatusTag :status="node.status === 'online'" on="在线" off="离线" />
        <Tag v-if="node.certRevokedAt" tone="danger" dot>证书已吊销</Tag>
      </div>
    </div>

    <div v-if="!node" class="band">
      <EmptyState :title="loading ? '加载中' : '节点不存在或无权访问'" desc="请返回节点列表确认节点仍在当前租户与数据范围内。" icon="server" />
    </div>

    <template v-else>
      <Panel v-show="tab === 'overview'" title="节点身份" meta="证书、分组与配置版本">
        <template #tools>
          <PermissionButton
            perm="device:node:write"
            variant="secondary"
            size="sm"
            danger
            icon="ban"
            label="吊销证书"
            :disabled="!canRevoke"
            @click="revokeCertificate"
          />
        </template>
        <div class="panel-body">
          <div class="kv-grid">
            <div class="kv">
              <div class="kv-label">状态</div>
              <div class="kv-value"><StatusTag :status="node.status === 'online'" on="在线" off="离线" /></div>
            </div>
            <div class="kv">
              <div class="kv-label">最后心跳</div>
              <div class="kv-value mono">{{ node.lastSeen || '—' }}</div>
            </div>
            <div class="kv">
              <div class="kv-label">Agent 版本</div>
              <div class="kv-value">{{ node.agentVersion || '—' }}</div>
            </div>
            <div class="kv">
              <div class="kv-label">分组</div>
              <div class="kv-value">{{ node.groupName || '未分组' }}</div>
            </div>
            <div class="kv">
              <div class="kv-label">证书指纹</div>
              <div class="kv-value node-fingerprint">
                <code>{{ fingerprintText }}</code>
                <Button v-if="node.certFingerprint" variant="text" size="sm" icon="copy" @click="copyFingerprint">复制</Button>
              </div>
            </div>
            <div class="kv">
              <div class="kv-label">证书到期</div>
              <div class="kv-value mono">{{ node.certNotAfter || '—' }}</div>
            </div>
            <div class="kv">
              <div class="kv-label">证书状态</div>
              <div class="kv-value">
                <Tag v-if="node.certRevokedAt" tone="danger" dot>已吊销 {{ node.certRevokedAt }}</Tag>
                <Tag v-else-if="node.certFingerprint" tone="ok" dot>有效</Tag>
                <span v-else class="faint">未绑定证书</span>
              </div>
            </div>
            <div class="kv">
              <div class="kv-label">配置版本</div>
              <div class="kv-value" style="display: flex; align-items: center; gap: 8px; flex-wrap: wrap">
                <span class="mono">期望 v{{ node.desiredConfigVersion }} / 实际 v{{ node.reportedConfigVersion }}</span>
                <Tag :tone="node.configInSync ? 'ok' : 'danger'" dot>{{ node.configInSync ? '已同步' : '配置漂移' }}</Tag>
              </div>
            </div>
            <div class="kv span-2">
              <div class="kv-label">标签</div>
              <div class="kv-value">{{ labelsText }}</div>
            </div>
          </div>
        </div>
      </Panel>

      <Panel v-show="tab === 'telemetry'" title="最新指标" :meta="`${latest.length} 项`">
        <template #tools>
          <Button v-if="telemetryPath" variant="secondary" size="sm" icon="activity" @click="openNodePage(telemetryPath)">查看时间序列</Button>
        </template>
        <div class="panel-body config-stack">
          <p v-if="telemetryError" class="form-error" style="margin: 0">{{ telemetryError }}</p>
          <DataTable :columns="telemetryColumns" :data="latest" :min-width="620" empty-text="该节点还没有上报遥测">
            <template #cell-metric="{ row }"><span class="mono">{{ row.metric }}</span></template>
            <template #cell-value="{ row }"><span class="tnum">{{ row.value }}</span></template>
            <template #cell-unit="{ row }"><span class="soft">{{ row.unit || '—' }}</span></template>
            <template #cell-collectedAt="{ row }"><span class="mono soft">{{ row.collectedAt }}</span></template>
          </DataTable>
        </div>
      </Panel>

      <Panel v-show="tab === 'alarms'" title="节点告警" :meta="`${alarms.length} 条`">
        <div class="panel-body config-stack">
          <p v-if="alarmsError" class="form-error" style="margin: 0">{{ alarmsError }}</p>
          <DataTable :columns="alarmColumns" :data="alarms" :min-width="820" empty-text="该节点暂无告警">
            <template #cell-level="{ row }"><Tag :tone="LEVEL_TONE[row.level] || 'neutral'" dot>{{ LEVEL_LABEL[row.level] || row.level }}</Tag></template>
            <template #cell-metric="{ row }"><span class="mono">{{ row.metric }}</span></template>
            <template #cell-message="{ row }"><span class="faint">{{ row.message || '—' }}</span></template>
            <template #cell-status="{ row }">
              <Tag :tone="ALARM_STATUS_TONE[row.status] || 'neutral'" dot>{{ ALARM_STATUS_LABEL[row.status] || row.status }}</Tag>
            </template>
            <template #cell-triggeredAt="{ row }"><span class="mono soft">{{ row.triggeredAt }}</span></template>
            <template #cell-actions="{ row }">
              <PermissionButton v-if="row.status === 'open'" perm="device:alarm:handle" variant="text" size="sm" icon="check" label="确认" @click="ackAlarm(row)" />
              <PermissionButton v-if="row.status !== 'resolved'" perm="device:alarm:handle" variant="text" size="sm" icon="power" label="恢复" @click="resolveAlarm(row)" />
            </template>
          </DataTable>
        </div>
      </Panel>

      <Panel v-show="tab === 'commands'" title="节点命令" :meta="`${commands.length} 条`">
        <template #tools>
          <Button v-if="commandsPath" variant="secondary" size="sm" icon="command" @click="openNodePage(commandsPath)">打开命令中心</Button>
        </template>
        <div class="panel-body config-stack">
          <p v-if="commandsError" class="form-error" style="margin: 0">{{ commandsError }}</p>
          <DataTable :columns="commandColumns" :data="commands" :min-width="760" empty-text="该节点还没有命令记录">
            <template #cell-type="{ row }"><span class="mono">{{ row.type }}</span></template>
            <template #cell-status="{ row }">
              <Tag :tone="COMMAND_STATUS_TONE[row.status] || 'neutral'" dot>{{ COMMAND_STATUS_LABEL[row.status] || row.status }}</Tag>
            </template>
            <template #cell-message="{ row }"><span class="faint">{{ row.message || '—' }}</span></template>
            <template #cell-createdAt="{ row }"><span class="mono soft">{{ row.createdAt }}</span></template>
            <template #cell-finishedAt="{ row }"><span class="mono soft">{{ row.finishedAt || '—' }}</span></template>
          </DataTable>
        </div>
      </Panel>

      <Panel v-show="tab === 'firmware'" title="固件命令" :meta="`${firmwareCommands.length} 条`">
        <template #tools>
          <Button v-if="firmwarePath" variant="secondary" size="sm" icon="layers" @click="openPage(firmwarePath)">打开固件页</Button>
        </template>
        <div class="panel-body config-stack">
          <p class="faint" style="margin: 0">这里只列出命令类型以 firmware. 开头的记录；固件任务由命令承载。</p>
          <p v-if="commandsError" class="form-error" style="margin: 0">{{ commandsError }}</p>
          <DataTable :columns="commandColumns" :data="firmwareCommands" :min-width="760" empty-text="该节点还没有固件升级记录">
            <template #cell-type="{ row }"><span class="mono">{{ row.type }}</span></template>
            <template #cell-status="{ row }">
              <Tag :tone="COMMAND_STATUS_TONE[row.status] || 'neutral'" dot>{{ COMMAND_STATUS_LABEL[row.status] || row.status }}</Tag>
            </template>
            <template #cell-message="{ row }"><span class="faint">{{ row.message || '—' }}</span></template>
            <template #cell-createdAt="{ row }"><span class="mono soft">{{ row.createdAt }}</span></template>
            <template #cell-finishedAt="{ row }"><span class="mono soft">{{ row.finishedAt || '—' }}</span></template>
          </DataTable>
        </div>
      </Panel>

      <NodeConfigPanel v-show="tab === 'config'" :node-id="nodeId" @updated="load" />
    </template>
  </div>
</template>
