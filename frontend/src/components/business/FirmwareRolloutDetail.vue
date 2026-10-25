<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { Button, DataTable, Drawer, Field, Panel, Tag, TextInput, type Column } from '@/components/ui'
import FormDialog from './FormDialog.vue'
import PermissionButton from './PermissionButton.vue'
import {
  deviceApi,
  fleetApi,
  type FirmwareRolloutAggregate,
  type FirmwareRolloutPolicy,
  type FirmwareRolloutTarget,
  type FirmwareRolloutWave,
  type FirmwareRow,
  type NodeRow,
} from '@/api'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'
import { formatLocalDateTime } from '@/utils/time'

const props = defineProps<{
  open: boolean
  rolloutId: number | null
}>()

const emit = defineEmits<{ (e: 'close'): void }>()

const ROLLOUT_STATUS: Record<string, { label: string; tone: string }> = {
  draft: { label: '草稿', tone: 'neutral' },
  scheduled: { label: '已排期', tone: 'neutral' },
  preparing: { label: '准备中', tone: 'info' },
  running: { label: '运行中', tone: 'info' },
  observing: { label: '观察中', tone: 'info' },
  paused: { label: '已暂停', tone: 'neutral' },
  rolling_back: { label: '回滚中', tone: 'info' },
  rollback_observing: { label: '回滚观察中', tone: 'info' },
  rolled_back: { label: '已回滚', tone: 'ok' },
  completed: { label: '已完成', tone: 'ok' },
  aborted: { label: '已中止', tone: 'neutral' },
  failed: { label: '已失败', tone: 'danger' },
  rollback_unavailable: { label: '回滚不可用', tone: 'danger' },
}

const WAVE_STATUS: Record<string, { label: string; tone: string }> = {
  pending: { label: '待派发', tone: 'neutral' },
  dispatching: { label: '派发中', tone: 'info' },
  observing: { label: '观察中', tone: 'info' },
  passed: { label: '已通过', tone: 'ok' },
  failed: { label: '失败', tone: 'danger' },
  skipped: { label: '已跳过', tone: 'neutral' },
  rolling_back: { label: '回滚中', tone: 'info' },
  rolled_back: { label: '已回滚', tone: 'ok' },
}

const TARGET_STATUS: Record<string, { label: string; tone: string }> = {
  pending: { label: '待派发', tone: 'neutral' },
  dispatching: { label: '派发中', tone: 'info' },
  dispatched: { label: '已派发', tone: 'info' },
  downloading: { label: '下载中', tone: 'info' },
  installing: { label: '安装中', tone: 'info' },
  installed: { label: '已安装', tone: 'info' },
  healthy: { label: '健康', tone: 'ok' },
  failed: { label: '失败', tone: 'danger' },
  unreachable: { label: '不可达', tone: 'danger' },
  rolling_back: { label: '回滚中', tone: 'info' },
  rolled_back: { label: '已回滚', tone: 'ok' },
  rollback_failed: { label: '回滚失败', tone: 'danger' },
  rollback_unavailable: { label: '回滚不可用', tone: 'danger' },
}

const GATE_LABEL: Record<string, string> = {
  pending: '待判定',
  installing: '观察中',
  healthy: '健康',
  failed: '失败',
  rolled_back: '已回滚',
  rollback_failed: '回滚失败',
  rollback_unavailable: '无法回滚',
}

const waveColumns: Column[] = [
  { key: 'waveIndex', title: '波次', width: 90 },
  { key: 'status', title: '状态', width: 110 },
  { key: 'targetCount', title: '目标', width: 80 },
  { key: 'dispatchedAt', title: '派发时间', width: 170 },
  { key: 'deadlineAt', title: '观察截止', width: 170 },
  { key: 'gate', title: '最近门控', minWidth: 240 },
]

const targetColumns: Column[] = [
  { key: 'nodeId', title: '节点', minWidth: 170 },
  { key: 'wave', title: '波次', width: 80 },
  { key: 'status', title: '状态', width: 110 },
  { key: 'installedVersion', title: '已安装版本', width: 150 },
  { key: 'previousVersion', title: '上一版本', minWidth: 170 },
  { key: 'gate', title: '门控原因', minWidth: 240 },
  { key: 'updatedAt', title: '更新时间', width: 170 },
]

const rollout = ref<FirmwareRolloutAggregate | null>(null)
const nodeRows = ref<NodeRow[]>([])
const firmwareRows = ref<FirmwareRow[]>([])
const loading = ref(false)
const loadError = ref('')
const acting = ref('')
const forceOpen = ref(false)
const forceReason = ref('')
const forceError = ref('')
let pollTimer: number | null = null
let requestInFlight = false
let metaLoaded = false

const status = computed(() => rollout.value?.status ?? '')
const terminal = computed(() => ['completed', 'rolled_back', 'aborted', 'failed', 'rollback_unavailable'].includes(status.value))
const canPause = computed(() => !terminal.value && ['scheduled', 'preparing', 'running', 'observing'].includes(status.value))
const canResume = computed(() => status.value === 'paused')
const canPromote = computed(() => ['running', 'paused'].includes(status.value))
const canAbort = computed(() => !terminal.value && ['scheduled', 'preparing', 'running', 'observing', 'paused'].includes(status.value))
const canRollback = computed(() => ['running', 'paused', 'failed'].includes(status.value))

const nodeMap = computed(() => new Map(nodeRows.value.map(node => [node.id, node])))
const firmwareMap = computed(() => new Map(firmwareRows.value.map(row => [row.id, row])))
const sub = computed(() => rollout.value
  ? `${rollout.value.name} · ${firmwareLabel(rollout.value.firmwareId)} · 每 5 秒自动刷新`
  : '')
const healthyCount = computed(() => rollout.value?.targets.filter(target => target.status === 'healthy').length ?? 0)

function rolloutStatus(value: string) {
  return ROLLOUT_STATUS[value] ?? { label: value || '未知', tone: 'neutral' }
}

function waveStatus(value: string) {
  return WAVE_STATUS[value] ?? { label: value || '未知', tone: 'neutral' }
}

function targetStatus(value: string) {
  return TARGET_STATUS[value] ?? { label: value || '未知', tone: 'neutral' }
}

function firmwareLabel(id: number) {
  if (!id) return '未记录'
  const firmware = firmwareMap.value.get(id)
  return firmware ? `${firmware.name} ${firmware.version}` : `固件 #${id}`
}

function nodeName(id: number) {
  return nodeMap.value.get(id)?.name ?? `节点 #${id}`
}

function installedVersion(target: FirmwareRolloutTarget) {
  return target.health?.installedVersion || nodeMap.value.get(target.nodeId)?.installedFirmwareVersion || '—'
}

function gateReason(target: FirmwareRolloutTarget) {
  const health = target.health ?? {}
  if (health.reason) return health.reason
  if (health.gate) return GATE_LABEL[health.gate] ?? health.gate
  return '—'
}

function waveGateText(wave: FirmwareRolloutWave) {
  const gates = wave.gate ?? []
  if (!gates.length) return '—'
  const latest = gates[gates.length - 1]
  if (latest.forced) return `强制通过${latest.reason ? `：${latest.reason}` : ''}`
  if (latest.timeout) return '超时'
  if (latest.passed) return `通过${typeof latest.healthy === 'number' ? `（健康 ${latest.healthy}/${latest.total ?? '—'}）` : ''}`
  if (latest.reason) return latest.reason
  return '—'
}

function waveNumber(waveId: number) {
  const wave = rollout.value?.waves.find(item => item.id === waveId)
  return wave ? `第 ${wave.waveIndex + 1} 波` : '—'
}

function currentWaveText(value: FirmwareRolloutAggregate) {
  if (['draft', 'scheduled', 'preparing'].includes(value.status)) return `未开始 / 共 ${value.totalWaves} 波`
  if (value.totalWaves <= 0) return '—'
  const index = Math.min(Math.max(value.currentWave, 0), value.totalWaves - 1)
  return `第 ${index + 1} / ${value.totalWaves} 波`
}

function wavePlan(policy?: FirmwareRolloutPolicy) {
  if (!policy?.waves?.length) return '—'
  return policy.waves.map(wave => (typeof wave.percent === 'number' ? `${wave.percent}%` : `${wave.size ?? '—'} 台`)).join(' → ')
}

function percentText(value?: number) {
  if (typeof value !== 'number' || !Number.isFinite(value)) return '—'
  return `${Math.round(value * 1000) / 10}%`
}

function timeoutText(policy?: FirmwareRolloutPolicy) {
  if (!policy) return '—'
  return policy.onTimeout === 'fail' ? '超时判失败' : '超时暂停'
}

function stopPolling() {
  if (pollTimer !== null) {
    window.clearInterval(pollTimer)
    pollTimer = null
  }
}

async function load(silent = false) {
  const targetId = props.rolloutId
  if (!props.open || !targetId || requestInFlight) return
  requestInFlight = true
  if (!silent) loading.value = true
  try {
    const data = await fleetApi.firmware.rollout(targetId)
    if (!props.open || props.rolloutId !== targetId) return
    // 轮询响应可能晚于人工动作的响应到达，版本较旧时不覆盖新状态
    if (rollout.value && rollout.value.id === data.id && data.version < rollout.value.version) return
    rollout.value = data
    loadError.value = ''
  } catch (error) {
    if (!silent) loadError.value = error instanceof Error ? error.message : '灰度发布加载失败'
  } finally {
    requestInFlight = false
    if (!silent) loading.value = false
  }
}

async function loadMeta() {
  if (metaLoaded) return
  try {
    const [nodePage, firmwarePage] = await Promise.all([
      deviceApi.nodes.list({ pageNum: 1, pageSize: 100 }),
      fleetApi.firmware.list({ pageNum: 1, pageSize: 100 }),
    ])
    nodeRows.value = nodePage.rows
    firmwareRows.value = firmwarePage.rows
    metaLoaded = true
  } catch {
    // 名称与版本只用于展示，加载失败时退回节点与固件标识
  }
}

watch([() => props.open, () => props.rolloutId], async ([open, id]) => {
  stopPolling()
  forceOpen.value = false
  if (!open || !id) return
  rollout.value = null
  loadError.value = ''
  await Promise.all([load(), loadMeta()])
  if (!props.open) return
  pollTimer = window.setInterval(() => {
    if (!acting.value) void load(true)
  }, 5000)
}, { immediate: true })

onBeforeUnmount(stopPolling)

async function runAction(key: string, action: (id: number) => Promise<FirmwareRolloutAggregate>, message: string) {
  if (!rollout.value || acting.value) return
  acting.value = key
  try {
    rollout.value = await action(rollout.value.id)
    pushToast(message)
  } catch (error) {
    toastError(error, '灰度操作失败')
  } finally {
    acting.value = ''
  }
}

async function pause() {
  await runAction('pause', id => fleetApi.firmware.pauseRollout(id), '灰度发布已暂停')
}

async function resume() {
  await runAction('resume', id => fleetApi.firmware.resumeRollout(id), '灰度发布已恢复')
}

async function promote() {
  const ok = await confirm({ title: '提升波次', message: '当前波次将直接判定通过并进入下一波，尚未完成的目标会被取消。', confirmText: '提升' })
  if (!ok) return
  await runAction('promote', id => fleetApi.firmware.promoteRollout(id), '已提升到下一波')
}

function openForce() {
  forceReason.value = ''
  forceError.value = ''
  forceOpen.value = true
}

async function submitForce() {
  if (!rollout.value || acting.value) return
  const reason = forceReason.value.trim()
  if (!reason) {
    forceError.value = '请填写强制提升原因'
    return
  }
  acting.value = 'force'
  forceError.value = ''
  try {
    rollout.value = await fleetApi.firmware.forcePromoteRollout(rollout.value.id, reason)
    forceOpen.value = false
    pushToast('已强制提升到下一波')
  } catch (error) {
    forceError.value = error instanceof Error ? error.message : '强制提升失败'
  } finally {
    acting.value = ''
  }
}

async function abort() {
  const ok = await confirm({
    title: '中止灰度发布',
    message: '中止会取消所有未完成目标，已安装的节点不会自动回滚。',
    confirmText: '中止',
    danger: true,
  })
  if (!ok) return
  await runAction('abort', id => fleetApi.firmware.abortRollout(id), '灰度发布已中止')
}

async function rollback() {
  const ok = await confirm({
    title: '回滚灰度发布',
    message: '将为已派发目标安排回滚到发布前版本，确认继续？',
    confirmText: '回滚',
    danger: true,
  })
  if (!ok) return
  await runAction('rollback', id => fleetApi.firmware.rollbackRollout(id), '灰度发布已进入回滚')
}
</script>

<template>
  <Drawer :open="open" title="灰度发布详情" :sub="sub" :width="1040" @close="emit('close')">
    <div class="stack">
      <p v-if="loadError && !rollout" class="form-error" role="alert">{{ loadError }}</p>
      <div v-if="!rollout && loading" class="faint">正在加载灰度发布…</div>
      <template v-if="rollout">
        <div class="kv-grid">
          <div class="kv">
            <div class="kv-label">状态</div>
            <div class="kv-value"><Tag :tone="rolloutStatus(rollout.status).tone" dot>{{ rolloutStatus(rollout.status).label }}</Tag></div>
          </div>
          <div class="kv">
            <div class="kv-label">当前波次</div>
            <div class="kv-value">{{ currentWaveText(rollout) }}</div>
          </div>
          <div class="kv">
            <div class="kv-label">目标节点</div>
            <div class="kv-value">{{ rollout.targets.length }} 个 · 健康 {{ healthyCount }}</div>
          </div>
          <div class="kv">
            <div class="kv-label">创建时间</div>
            <div class="kv-value mono">{{ formatLocalDateTime(rollout.createdAt) }}</div>
          </div>
          <div v-if="rollout.pauseReason" class="kv span-2">
            <div class="kv-label">暂停原因</div>
            <div class="kv-value">{{ rollout.pauseReason }}</div>
          </div>
          <div v-if="rollout.lastError" class="kv span-2">
            <div class="kv-label">最近失败原因</div>
            <div class="kv-value">{{ rollout.lastError }}</div>
          </div>
        </div>

        <Panel title="波次" :meta="`共 ${rollout.totalWaves} 波`">
          <DataTable :columns="waveColumns" :data="rollout.waves" :min-width="860" empty-text="暂无波次">
            <template #cell-waveIndex="{ row }">第 {{ row.waveIndex + 1 }} 波</template>
            <template #cell-status="{ row }">
              <Tag :tone="waveStatus(row.status).tone" dot>{{ waveStatus(row.status).label }}</Tag>
            </template>
            <template #cell-targetCount="{ row }"><span class="tnum">{{ row.targetCount }} 个</span></template>
            <template #cell-dispatchedAt="{ row }"><span class="mono soft">{{ formatLocalDateTime(row.dispatchedAt) }}</span></template>
            <template #cell-deadlineAt="{ row }"><span class="mono soft">{{ formatLocalDateTime(row.deadlineAt) }}</span></template>
            <template #cell-gate="{ row }"><span class="soft">{{ waveGateText(row) }}</span></template>
          </DataTable>
        </Panel>

        <Panel title="目标节点" :meta="`共 ${rollout.targets.length} 个`">
          <DataTable :columns="targetColumns" :data="rollout.targets" :min-width="1100" empty-text="暂无目标节点">
            <template #cell-nodeId="{ row }">
              <div class="cell-title">{{ nodeName(row.nodeId) }}</div>
              <div class="cell-sub mono">#{{ row.nodeId }}</div>
            </template>
            <template #cell-wave="{ row }">{{ waveNumber(row.waveId) }}</template>
            <template #cell-status="{ row }">
              <Tag :tone="targetStatus(row.status).tone" dot>{{ targetStatus(row.status).label }}</Tag>
            </template>
            <template #cell-installedVersion="{ row }"><span class="mono soft">{{ installedVersion(row) }}</span></template>
            <template #cell-previousVersion="{ row }">
              <span :class="row.previousFirmwareId ? 'mono soft' : 'faint'">{{ firmwareLabel(row.previousFirmwareId) }}</span>
            </template>
            <template #cell-gate="{ row }"><span class="soft">{{ gateReason(row) }}</span></template>
            <template #cell-updatedAt="{ row }"><span class="mono soft">{{ formatLocalDateTime(row.updatedAt) }}</span></template>
          </DataTable>
        </Panel>

        <Panel title="发布策略">
          <div class="kv-grid policy-grid">
            <div class="kv">
              <div class="kv-label">波次计划</div>
              <div class="kv-value">{{ wavePlan(rollout.policy) }}</div>
            </div>
            <div class="kv">
              <div class="kv-label">每波并发上限</div>
              <div class="kv-value">{{ rollout.policy?.maxConcurrentPerWave ? `${rollout.policy.maxConcurrentPerWave} 个` : '不限' }}</div>
            </div>
            <div class="kv">
              <div class="kv-label">沉降 / 观察 / 单波上限</div>
              <div class="kv-value">{{ rollout.policy?.settleSeconds ?? '—' }} / {{ rollout.policy?.observeSeconds ?? '—' }} / {{ rollout.policy?.maxWaveSeconds ?? '—' }} 秒</div>
            </div>
            <div class="kv">
              <div class="kv-label">最低成功率（单波 / 累计）</div>
              <div class="kv-value">{{ percentText(rollout.policy?.minWaveSuccessRatio) }} / {{ percentText(rollout.policy?.minCumulativeSuccessRatio) }}</div>
            </div>
            <div class="kv">
              <div class="kv-label">最大失败数</div>
              <div class="kv-value">{{ rollout.policy?.maxFailures ? `${rollout.policy.maxFailures} 个` : '不设上限' }}</div>
            </div>
            <div class="kv">
              <div class="kv-label">超时 / 自动回滚</div>
              <div class="kv-value">{{ timeoutText(rollout.policy) }} / {{ rollout.policy?.autoRollback ? '自动回滚' : '不自动回滚' }}</div>
            </div>
            <div class="kv">
              <div class="kv-label">遥测新鲜度</div>
              <div class="kv-value">{{ rollout.policy?.requiredFreshTelemetrySeconds ?? '—' }} 秒</div>
            </div>
            <div class="kv">
              <div class="kv-label">已安装版本校验</div>
              <div class="kv-value">{{ rollout.policy?.requireInstalledVersion ? '要求上报且与目标一致' : '未要求上报' }}</div>
            </div>
          </div>
        </Panel>
      </template>
    </div>

    <template #footer>
      <div class="rollout-foot">
        <span v-if="acting" class="faint">操作中…</span>
        <Button variant="ghost" icon="refresh" :loading="loading" @click="load()">刷新</Button>
        <PermissionButton v-if="canPause" perm="device:firmware:upload" icon="pause" label="暂停" :loading="acting === 'pause'" @click="pause" />
        <PermissionButton v-if="canResume" perm="device:firmware:upload" variant="primary" icon="play" label="恢复" :loading="acting === 'resume'" @click="resume" />
        <PermissionButton v-if="canPromote" perm="device:firmware:upload" icon="arrowUp" label="提升" :loading="acting === 'promote'" @click="promote" />
        <PermissionButton v-if="canPromote" perm="device:firmware:upload" variant="text" danger icon="alert" label="强制提升" @click="openForce" />
        <PermissionButton v-if="canAbort" perm="device:firmware:upload" variant="text" danger icon="ban" label="中止" :loading="acting === 'abort'" @click="abort" />
        <PermissionButton v-if="canRollback" perm="device:firmware:upload" variant="text" danger icon="refresh" label="回滚" :loading="acting === 'rollback'" @click="rollback" />
      </div>
    </template>
  </Drawer>

  <FormDialog
    :open="forceOpen"
    title="强制提升"
    sub="强制提升会跳过当前波尚未完成的目标，原因会写入门控记录。"
    :saving="acting === 'force'"
    submit-text="确认提升"
    @close="forceOpen = false"
    @submit="submitForce"
  >
    <Field label="原因" required span>
      <TextInput v-model="forceReason" multiline :rows="3" :maxlength="200" placeholder="例如 现场运维确认新版本运行正常" />
    </Field>
    <p v-if="forceError" class="form-error" role="alert">{{ forceError }}</p>
  </FormDialog>
</template>

<style scoped>
.rollout-foot {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: 8px;
  width: 100%;
}

.policy-grid {
  border: 0;
  background: transparent;
  gap: 12px;
  padding-bottom: 16px;
}

.policy-grid .kv {
  border: 1px solid var(--line);
  border-radius: var(--r-md);
}
</style>
