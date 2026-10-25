<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue'
import { Button, Checkbox, Field, Select, Switch, TextInput } from '@/components/ui'
import FormDialog from './FormDialog.vue'
import { fleetApi, type FirmwareRolloutAggregate, type FirmwareRolloutPolicy, type FirmwareRow, type NodeRow } from '@/api'
import { pushToast } from '@/composables/toast'

const props = withDefaults(defineProps<{
  open: boolean
  firmware: FirmwareRow | null
  nodes: NodeRow[]
  nodesLoading?: boolean
}>(), {
  nodesLoading: false,
})

const emit = defineEmits<{
  (e: 'close'): void
  (e: 'created', rollout: FirmwareRolloutAggregate): void
}>()

const MAX_WAVES = 50
const MAX_SECONDS = 604800
const MAX_CONCURRENT = 1000
const MAX_FAILURES = 1000000

const form = reactive({
  name: '',
  nodeIds: [] as number[],
  waves: [5, 25, 100] as number[],
  maxConcurrentPerWave: 0,
  settleSeconds: 60,
  observeSeconds: 120,
  maxWaveSeconds: 1800,
  minWaveSuccessRatio: 100,
  minCumulativeSuccessRatio: 100,
  maxFailures: 0,
  autoRollback: false,
  onTimeout: 'pause' as 'pause' | 'fail',
  requiredFreshTelemetrySeconds: 300,
  requireInstalledVersion: true,
})

const saving = ref(false)
const formError = ref('')

const timeoutOptions = [
  { label: '超时暂停', value: 'pause' },
  { label: '超时判失败', value: 'fail' },
]

function resetForm() {
  form.name = props.firmware ? `${props.firmware.name} ${props.firmware.version} 灰度` : ''
  form.nodeIds = []
  form.waves = [5, 25, 100]
  form.maxConcurrentPerWave = 0
  form.settleSeconds = 60
  form.observeSeconds = 120
  form.maxWaveSeconds = 1800
  form.minWaveSuccessRatio = 100
  form.minCumulativeSuccessRatio = 100
  form.maxFailures = 0
  form.autoRollback = false
  form.onTimeout = 'pause'
  form.requiredFreshTelemetrySeconds = 300
  form.requireInstalledVersion = true
  formError.value = ''
}

watch(() => props.open, open => {
  if (open) resetForm()
}, { immediate: true })

function toggleNode(id: number, checked: boolean) {
  form.nodeIds = checked ? [...form.nodeIds, id] : form.nodeIds.filter(nodeId => nodeId !== id)
}

function addWave() {
  if (form.waves.length >= MAX_WAVES) return
  const last = integerOf(form.waves[form.waves.length - 1])
  const beforeLast = form.waves.length > 1 ? integerOf(form.waves[form.waves.length - 2]) : 0
  const middle = Math.max(beforeLast + 1, Math.round((beforeLast + last) / 2))
  form.waves.splice(form.waves.length - 1, 0, Math.min(last, middle))
}

function removeWave(index: number) {
  if (form.waves.length <= 1) return
  form.waves.splice(index, 1)
}

function integerOf(value: unknown): number {
  const number = Number(value)
  return Number.isInteger(number) ? number : Number.NaN
}

function checkInteger(label: string, value: unknown, min: number, max: number): string {
  const number = integerOf(value)
  if (!Number.isInteger(number) || number < min || number > max) return `${label}必须是 ${min} 到 ${max} 的整数`
  return ''
}

function checkRatio(label: string, value: unknown): string {
  if (value === null || value === undefined || String(value).trim() === '') return `${label}不能为空`
  const number = Number(value)
  if (!Number.isFinite(number) || number < 0 || number > 100) return `${label}必须是 0 到 100 的百分比`
  return ''
}

/* 空值不能落成 0，否则会被当成 0% 绕过健康门控；返回 NaN 交给校验拦截 */
function ratioOf(value: unknown): number {
  if (value === null || value === undefined || String(value).trim() === '') return Number.NaN
  const number = Number(value)
  return Number.isFinite(number) ? number / 100 : Number.NaN
}

function validate(): string {
  if (!props.firmware) return '请选择要发布的固件'
  const name = form.name.trim()
  if (!name) return '请填写发布名称'
  if (new TextEncoder().encode(name).length > 128) return '发布名称不能超过 128 字节'
  if (!form.nodeIds.length) return '请至少选择一个目标节点'
  if (form.nodeIds.length > 1000) return '目标节点不能超过 1000 个'
  if (!form.waves.length) return '至少需要一个波次'
  if (form.waves.length > MAX_WAVES) return `波次数量不能超过 ${MAX_WAVES}`

  let previous = 0
  for (let index = 0; index < form.waves.length; index += 1) {
    const percent = integerOf(form.waves[index])
    if (!Number.isInteger(percent) || percent < 1 || percent > 100) return `第 ${index + 1} 波的累计百分比必须是 1 到 100 的整数`
    if (percent < previous) return '波次百分比必须按累计比例递增'
    previous = percent
  }
  if (previous !== 100) return '最后一波累计百分比必须为 100%，才能覆盖全部目标节点'

  const integerChecks = [
    checkInteger('每波并发上限', form.maxConcurrentPerWave, 0, MAX_CONCURRENT),
    checkInteger('沉降等待', form.settleSeconds, 1, MAX_SECONDS),
    checkInteger('观察窗口', form.observeSeconds, 0, MAX_SECONDS),
    checkInteger('单波最长时长', form.maxWaveSeconds, 1, MAX_SECONDS),
    checkInteger('最大失败数', form.maxFailures, 0, MAX_FAILURES),
    checkInteger('遥测新鲜度', form.requiredFreshTelemetrySeconds, 1, MAX_SECONDS),
    checkRatio('单波最低成功率', form.minWaveSuccessRatio),
    checkRatio('累计最低成功率', form.minCumulativeSuccessRatio),
  ].find(message => Boolean(message))
  if (integerChecks) return integerChecks

  const settle = Number(form.settleSeconds)
  const observe = Number(form.observeSeconds)
  const maxWave = Number(form.maxWaveSeconds)
  if (maxWave < Math.max(settle, observe)) return '单波最长时长不能小于沉降等待与观察窗口中的较大值'
  return ''
}

function policyPayload(): FirmwareRolloutPolicy {
  return {
    waves: form.waves.map(percent => ({ percent: integerOf(percent) })),
    maxConcurrentPerWave: integerOf(form.maxConcurrentPerWave),
    settleSeconds: integerOf(form.settleSeconds),
    observeSeconds: integerOf(form.observeSeconds),
    maxWaveSeconds: integerOf(form.maxWaveSeconds),
    minWaveSuccessRatio: ratioOf(form.minWaveSuccessRatio),
    minCumulativeSuccessRatio: ratioOf(form.minCumulativeSuccessRatio),
    maxFailures: integerOf(form.maxFailures),
    autoRollback: form.autoRollback,
    onTimeout: form.onTimeout === 'fail' ? 'fail' : 'pause',
    requiredFreshTelemetrySeconds: integerOf(form.requiredFreshTelemetrySeconds),
    exemptAlarmMetrics: [],
    requireInstalledVersion: form.requireInstalledVersion,
  }
}

function summaryWave(value: unknown) {
  const text = String(value ?? '').trim()
  return text === '' ? '—' : `${text}%`
}

function summaryRatio(value: unknown) {
  const text = String(value ?? '').trim()
  return text === '' ? '未填写' : `${text}%`
}

const summaryNodes = computed(() => form.nodeIds
  .map(id => props.nodes.find(node => node.id === id)?.name ?? `节点 #${id}`)
  .join(' → ') || '未选择')
const summaryWaves = computed(() => form.waves.map(summaryWave).join(' → '))

async function submit() {
  if (saving.value || !props.firmware) return
  const error = validate()
  if (error) {
    formError.value = error
    return
  }
  saving.value = true
  formError.value = ''
  try {
    const created = await fleetApi.firmware.createRollout(props.firmware.id, {
      name: form.name.trim(),
      nodeIds: [...form.nodeIds],
      policy: policyPayload(),
    })
    pushToast('灰度发布已创建')
    emit('created', created)
  } catch (error) {
    formError.value = error instanceof Error ? error.message : '灰度发布创建失败'
  } finally {
    saving.value = false
  }
}

function close() {
  if (saving.value) return
  emit('close')
}

function nodeVersion(node: NodeRow) {
  return node.installedFirmwareVersion ? `当前 ${node.installedFirmwareVersion}` : '版本未上报'
}
</script>

<template>
  <FormDialog
    :open="open"
    title="新建灰度发布"
    :sub="firmware ? `${firmware.name} ${firmware.version} · 按波次逐步放量，门控通过后自动推进` : ''"
    :saving="saving"
    :width="820"
    submit-text="创建并查看"
    @close="close"
    @submit="submit"
  >
    <Field label="发布名称" required span>
      <TextInput v-model="form.name" placeholder="例如 生产环境灰度" :maxlength="128" />
    </Field>
    <Field label="目标节点" required group span :hint="`已选 ${form.nodeIds.length} 个节点；节点顺序决定波次归属`">
      <div v-if="nodesLoading" class="node-empty">节点加载中…</div>
      <div v-else-if="!nodes.length" class="node-empty">没有可选节点</div>
      <div v-else class="node-picker">
        <label v-for="node in nodes" :key="node.id" class="node-picker-row">
          <Checkbox :model-value="form.nodeIds.includes(node.id)" :label="node.name" @update:model-value="toggleNode(node.id, $event)" />
          <span class="node-meta">
            <span>{{ node.status === 'online' ? '在线' : '离线' }}</span>
            <span class="faint">{{ nodeVersion(node) }}</span>
          </span>
        </label>
      </div>
    </Field>
    <Field label="波次百分比" required group span hint="按累计覆盖比例解释，例如 5 / 25 / 100 表示先覆盖 5%，再累计到 25%，最后铺满；最后一波必须为 100%">
      <div class="wave-list">
        <div v-for="(_, index) in form.waves" :key="index" class="wave-row">
          <label class="wave-main">
            <span class="wave-label">第 {{ index + 1 }} 波累计</span>
            <TextInput v-model="form.waves[index]" type="number" />
            <span class="wave-unit">%</span>
          </label>
          <Button variant="text" size="sm" danger :disabled="form.waves.length <= 1" @click="removeWave(index)">移除</Button>
        </div>
        <div class="wave-add">
          <Button variant="secondary" size="sm" icon="plus" :disabled="form.waves.length >= MAX_WAVES" @click="addWave">添加波次</Button>
          <span class="faint">最多 {{ MAX_WAVES }} 波</span>
        </div>
      </div>
    </Field>
    <Field label="每波并发上限" hint="0 表示不限，最大 1000">
      <TextInput v-model="form.maxConcurrentPerWave" type="number" />
    </Field>
    <Field label="沉降等待（秒）" required hint="派发后等待心跳与遥测的最短时间，服务端要求不小于两个心跳周期">
      <TextInput v-model="form.settleSeconds" type="number" />
    </Field>
    <Field label="观察窗口（秒）" required hint="沉降结束后继续观察健康与告警的时间">
      <TextInput v-model="form.observeSeconds" type="number" />
    </Field>
    <Field label="单波最长时长（秒）" required hint="超过后按超时动作处理">
      <TextInput v-model="form.maxWaveSeconds" type="number" />
    </Field>
    <Field label="单波最低成功率（%）" required hint="本波健康目标占已判定目标的最低比例">
      <TextInput v-model="form.minWaveSuccessRatio" type="number" />
    </Field>
    <Field label="累计最低成功率（%）" required hint="全部已派发目标的最低健康比例">
      <TextInput v-model="form.minCumulativeSuccessRatio" type="number" />
    </Field>
    <Field label="最大失败数" hint="0 表示不设上限">
      <TextInput v-model="form.maxFailures" type="number" />
    </Field>
    <Field label="遥测新鲜度（秒）" required hint="超过该年龄的遥测不能用于门控">
      <TextInput v-model="form.requiredFreshTelemetrySeconds" type="number" />
    </Field>
    <Field label="超时动作">
      <Select v-model="form.onTimeout" :options="timeoutOptions" />
    </Field>
    <Field label="失败自动回滚" group>
      <Switch v-model="form.autoRollback" label="门控失败或超时后自动回滚已派发目标" />
    </Field>
    <Field label="已安装版本校验" group>
      <Switch v-model="form.requireInstalledVersion" label="要求节点上报已安装版本并与目标版本一致" />
    </Field>
    <div class="rollout-summary" aria-label="创建摘要">
      <div class="summary-row">
        <span class="summary-label">固件</span>
        <span class="summary-value" :title="firmware ? `${firmware.name} ${firmware.version}` : ''">{{ firmware ? `${firmware.name} ${firmware.version}` : '未选择' }}</span>
      </div>
      <div class="summary-row">
        <span class="summary-label">目标节点</span>
        <span class="summary-value" :title="summaryNodes">{{ form.nodeIds.length }} 个 · {{ summaryNodes }}</span>
      </div>
      <div class="summary-row">
        <span class="summary-label">波次</span>
        <span class="summary-value">{{ summaryWaves }}（累计覆盖）</span>
      </div>
      <div class="summary-row">
        <span class="summary-label">成功率</span>
        <span class="summary-value">单波 {{ summaryRatio(form.minWaveSuccessRatio) }} / 累计 {{ summaryRatio(form.minCumulativeSuccessRatio) }}</span>
      </div>
      <div class="summary-row">
        <span class="summary-label">超时 / 回滚</span>
        <span class="summary-value">单波最长 {{ form.maxWaveSeconds }} 秒 · {{ form.onTimeout === 'fail' ? '超时判失败' : '超时暂停' }} · {{ form.autoRollback ? '自动回滚' : '不自动回滚' }}</span>
      </div>
    </div>
    <p v-if="formError" class="form-error" role="alert">{{ formError }}</p>
  </FormDialog>
</template>

<style scoped>
.node-picker {
  display: grid;
  gap: 6px;
  max-height: 240px;
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

.node-meta {
  display: flex;
  align-items: baseline;
  gap: 8px;
  margin-left: auto;
  font-size: 12px;
}

.node-empty {
  padding: 16px;
  border: 1px dashed var(--line);
  border-radius: 8px;
  text-align: center;
  color: var(--ink-soft);
  font-size: 12px;
}

.wave-list {
  display: grid;
  gap: 8px;
}

.wave-row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.wave-main {
  display: flex;
  align-items: center;
  gap: 8px;
}

.wave-label {
  width: 96px;
  flex: none;
  font-size: 12px;
  color: var(--ink-soft);
}

.wave-row :deep(.input-shell) {
  width: 120px;
}

.wave-unit {
  font-size: 12px;
  color: var(--ink-soft);
}

.wave-add {
  display: flex;
  align-items: center;
  gap: 10px;
}

.rollout-summary {
  display: grid;
  gap: 6px;
  grid-column: 1 / -1;
  padding: 12px 14px;
  border: 1px solid var(--line);
  border-radius: var(--r-md);
  background: var(--surface-solid);
}

.summary-row {
  display: flex;
  align-items: baseline;
  gap: 12px;
  font-size: 12px;
}

.summary-label {
  flex: none;
  width: 84px;
  color: var(--ink-soft);
}

.summary-value {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
</style>
