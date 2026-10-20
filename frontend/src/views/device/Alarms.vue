<script setup lang="ts">
import { onMounted, reactive, ref } from 'vue'
import { Drawer, DataTable, Field, Metric, Select, StatusTag, Switch, Tag, TextInput, type Column } from '@/components/ui'
import { FormDialog, ListPage, PermissionButton } from '@/components/business'
import { fleetApi, type AlarmRuleRow, type AlarmRow } from '@/api'
import { useListPage } from '@/composables/list'
import { useNodeSocket } from '@/composables/nodeSocket'
import { confirm } from '@/composables/confirm'
import { pushToast, toastError } from '@/composables/toast'

const LEVELS = [
  { label: '全部级别', value: '' },
  { label: '严重', value: 'critical' },
  { label: '警告', value: 'warning' },
  { label: '提示', value: 'info' },
]
const STATUSES = [
  { label: '全部状态', value: '' },
  { label: '未恢复', value: 'open' },
  { label: '已确认', value: 'acknowledged' },
  { label: '已恢复', value: 'resolved' },
]
const LEVEL_TONE: Record<string, string> = { critical: 'danger', warning: 'neutral', info: 'info' }
const LEVEL_LABEL: Record<string, string> = { critical: '严重', warning: '警告', info: '提示' }
const STATUS_LABEL: Record<string, string> = { open: '未恢复', acknowledged: '已确认', resolved: '已恢复' }

const openCount = ref(0)
const list = useListPage<AlarmRow, { status: string; level: string }>({
  query: { status: '', level: '' },
  fetch: async params => {
    const page = await fleetApi.alarms.list(params)
    openCount.value = page.openCount
    return page
  },
})
const socket = useNodeSocket(() => undefined, { onAlarm: () => list.load() })

const columns: Column[] = [
  { key: 'level', title: '级别', width: 90 },
  { key: 'nodeName', title: '节点', minWidth: 150 },
  { key: 'metric', title: '指标', minWidth: 140 },
  { key: 'value', title: '触发值', width: 100, align: 'right' },
  { key: 'threshold', title: '阈值', width: 90, align: 'right' },
  { key: 'status', title: '状态', width: 100 },
  { key: 'triggeredAt', title: '触发时间', width: 170 },
  { key: 'actions', title: '操作', width: 160, className: 'col-actions' },
]

async function ack(row: AlarmRow) {
  try {
    await fleetApi.alarms.ack(row.id)
    pushToast('告警已确认')
    await list.load()
  } catch (error) {
    toastError(error, '确认失败')
  }
}

async function resolve(row: AlarmRow) {
  const ok = await confirm({ title: '恢复告警', message: `确认把「${row.nodeName} · ${row.metric}」标记为已恢复吗？`, confirmText: '恢复' })
  if (!ok) return
  try {
    await fleetApi.alarms.resolve(row.id)
    pushToast('告警已恢复')
    await list.load()
  } catch (error) {
    toastError(error, '恢复失败')
  }
}

/* ── 规则 ── */
const rulesOpen = ref(false)
const rules = ref<AlarmRuleRow[]>([])
const rulesLoading = ref(false)
const ruleColumns: Column[] = [
  { key: 'name', title: '规则', minWidth: 180 },
  { key: 'metric', title: '指标' },
  { key: 'condition', title: '条件' },
  { key: 'level', title: '级别', width: 90 },
  { key: 'enabled', title: '启用', width: 80 },
  { key: 'actions', title: '操作', width: 120, className: 'col-actions' },
]
async function loadRules() {
  rulesLoading.value = true
  try {
    rules.value = (await fleetApi.alarms.rules({ pageNum: 1, pageSize: 100 })).rows
  } catch (error) {
    toastError(error, '规则加载失败')
  } finally {
    rulesLoading.value = false
  }
}
function openRules() {
  rulesOpen.value = true
  loadRules()
}

interface RuleForm { id?: number; name: string; metric: string; comparison: string; threshold: number | null; level: string; enabled: boolean; cooldownSeconds: number | null }
const ruleDialog = ref(false)
const ruleSaving = ref(false)
const ruleForm = reactive<RuleForm>({ name: '', metric: 'cpu.usage', comparison: '>', threshold: 90, level: 'warning', enabled: true, cooldownSeconds: 300 })
const ruleError = ref('')
const COMPARISONS = ['>', '>=', '<', '<=', '=='].map(value => ({ label: value, value }))
const RULE_LEVELS = LEVELS.slice(1)

function openRuleCreate() {
  Object.assign(ruleForm, { id: undefined, name: '', metric: 'cpu.usage', comparison: '>', threshold: 90, level: 'warning', enabled: true, cooldownSeconds: 300 })
  ruleError.value = ''
  ruleDialog.value = true
}
function openRuleEdit(rule: AlarmRuleRow) {
  Object.assign(ruleForm, { id: rule.id, name: rule.name, metric: rule.metric, comparison: rule.comparison, threshold: rule.threshold, level: rule.level, enabled: rule.enabled, cooldownSeconds: rule.cooldownSeconds })
  ruleError.value = ''
  ruleDialog.value = true
}
async function saveRule() {
  if (!ruleForm.name.trim() || !ruleForm.metric.trim() || ruleForm.threshold === null) {
    ruleError.value = '规则名、指标与阈值都不能为空'
    return
  }
  ruleSaving.value = true
  const payload = { name: ruleForm.name.trim(), nodeId: 0, metric: ruleForm.metric.trim(), comparison: ruleForm.comparison, threshold: ruleForm.threshold, level: ruleForm.level, enabled: ruleForm.enabled, cooldownSeconds: ruleForm.cooldownSeconds ?? 300 }
  try {
    if (ruleForm.id) await fleetApi.alarms.updateRule(ruleForm.id, payload)
    else await fleetApi.alarms.createRule(payload)
    pushToast('规则已保存')
    ruleDialog.value = false
    await loadRules()
  } catch (error) {
    toastError(error, '规则保存失败')
  } finally {
    ruleSaving.value = false
  }
}
async function removeRule(rule: AlarmRuleRow) {
  const ok = await confirm({ title: '删除规则', message: `确认删除「${rule.name}」吗？`, confirmText: '删除', danger: true })
  if (!ok) return
  try {
    await fleetApi.alarms.removeRule(rule.id)
    pushToast('规则已删除')
    await loadRules()
  } catch (error) {
    toastError(error, '规则删除失败')
  }
}
async function toggleRule(rule: AlarmRuleRow, enabled: boolean) {
  try {
    await fleetApi.alarms.updateRule(rule.id, { enabled })
    rule.enabled = enabled
    pushToast(enabled ? '规则已启用' : '规则已停用')
  } catch (error) {
    toastError(error, '规则更新失败')
  }
}
onMounted(loadRules)
</script>

<template>
  <div :data-ws-state="socket.state">
    <ListPage title="告警中心" desc="遥测触发告警、确认与恢复；规则命中按冷却窗口去重。" table-title="告警列表" :list="list" :columns="columns" :min-width="1100">
      <template #before>
        <section class="band reveal">
          <div class="band-head"><span class="band-label">当前未恢复</span></div>
          <div class="readout-strip">
            <Metric label="未恢复告警" :value="openCount" unit="条" :class="{ 'is-alert': openCount }" />
            <Metric label="实时通道" :value="socket.state.value === 'connected' ? 1 : 0" unit="路" />
          </div>
        </section>
      </template>
      <template #search>
        <Select v-model="list.query.status" :options="STATUSES" placeholder="全部状态" />
        <Select v-model="list.query.level" :options="LEVELS" placeholder="全部级别" />
      </template>
      <template #tools>
        <PermissionButton perm="device:alarm:add" icon="sliders" label="告警规则" @click="openRules" />
      </template>
      <template #cell-level="{ row }"><Tag :tone="LEVEL_TONE[row.level] || 'neutral'" dot>{{ LEVEL_LABEL[row.level] || row.level }}</Tag></template>
      <template #cell-metric="{ row }"><span class="mono">{{ row.metric }}</span></template>
      <template #cell-value="{ row }"><span class="tnum">{{ row.value }}</span></template>
      <template #cell-threshold="{ row }"><span class="tnum soft">{{ row.threshold }}</span></template>
      <template #cell-status="{ row }">
        <StatusTag :status="row.status !== 'open'" on="已处理" off="未恢复" />
        <span class="cell-sub">{{ STATUS_LABEL[row.status] || row.status }}</span>
      </template>
      <template #cell-triggeredAt="{ row }"><span class="mono soft">{{ row.triggeredAt }}</span></template>
      <template #cell-actions="{ row }">
        <PermissionButton v-if="row.status === 'open'" perm="device:alarm:handle" variant="text" size="sm" icon="check" label="确认" @click="ack(row)" />
        <PermissionButton v-if="row.status !== 'resolved'" perm="device:alarm:handle" variant="text" size="sm" icon="power" label="恢复" @click="resolve(row)" />
      </template>
    </ListPage>

    <Drawer :open="rulesOpen" title="告警规则" sub="规则按指标与阈值评估；同一节点恢复后进入冷却窗口。" :width="760" @close="rulesOpen = false">
      <div class="drawer-actions">
        <PermissionButton perm="device:alarm:add" icon="plus" label="新增规则" @click="openRuleCreate" />
      </div>
      <DataTable :columns="ruleColumns" :data="rules" :loading="rulesLoading" :min-width="680" empty-text="还没有告警规则">
        <template #cell-condition="{ row }"><span class="mono">{{ row.comparison }} {{ row.threshold }}</span></template>
        <template #cell-level="{ row }"><Tag :tone="LEVEL_TONE[row.level] || 'neutral'">{{ LEVEL_LABEL[row.level] || row.level }}</Tag></template>
        <template #cell-enabled="{ row }"><Switch :model-value="row.enabled" @update:model-value="(value: boolean) => toggleRule(row, value)" /></template>
        <template #cell-actions="{ row }">
          <PermissionButton perm="device:alarm:edit" variant="text" size="sm" icon="edit" label="编辑" @click="openRuleEdit(row)" />
          <PermissionButton perm="device:alarm:remove" variant="text" size="sm" danger icon="trash" label="删除" @click="removeRule(row)" />
        </template>
      </DataTable>
    </Drawer>

    <FormDialog :open="ruleDialog" title="告警规则" sub="阈值与比较符共同决定触发条件。" :saving="ruleSaving" @close="ruleDialog = false" @submit="saveRule">
      <Field label="规则名称" required><TextInput v-model="ruleForm.name" placeholder="例如 CPU 使用率过高" /></Field>
      <Field label="指标" required><TextInput v-model="ruleForm.metric" placeholder="cpu.usage" /></Field>
      <Field label="比较符"><Select v-model="ruleForm.comparison" :options="COMPARISONS" /></Field>
      <Field label="阈值" required><TextInput v-model.number="ruleForm.threshold" placeholder="90" /></Field>
      <Field label="级别"><Select v-model="ruleForm.level" :options="RULE_LEVELS" /></Field>
      <Field label="冷却秒数"><TextInput v-model.number="ruleForm.cooldownSeconds" placeholder="300" /></Field>
      <Field label="启用"><Switch v-model="ruleForm.enabled" /></Field>
      <p v-if="ruleError" class="form-error">{{ ruleError }}</p>
    </FormDialog>
  </div>
</template>
