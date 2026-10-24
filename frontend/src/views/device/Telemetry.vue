<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { EmptyState, Metric, SectionLabel, Select, Tag } from '@/components/ui'
import TrendChart from '@/components/charts/TrendChart.vue'
import { deviceApi, fleetApi, type NodeRow, type TelemetryPoint } from '@/api'
import { toastError } from '@/composables/toast'
import { formatMetricValue } from '@/utils/metric'

const route = useRoute()
const nodes = ref<NodeRow[]>([])
// 从节点详情跳转时带 nodeId；没有合法参数才回落到第一个节点
const queryNodeId = Number(route.query.nodeId)
const nodeId = ref<number | null>(Number.isInteger(queryNodeId) && queryNodeId > 0 ? queryNodeId : null)
const nodeOptions = computed(() => nodes.value.map(node => ({ label: `${node.name} · ${node.status === 'online' ? '在线' : '离线'}`, value: node.id })))
const latest = ref<TelemetryPoint[]>([])
const metric = ref('')
const metricOptions = computed(() => latest.value.map(point => ({ label: `${point.metric}${point.unit ? ` (${point.unit})` : ''}`, value: point.metric })))
const range = ref('24h')
const ranges = [
  { label: '近 1 小时', value: '1h' },
  { label: '近 6 小时', value: '6h' },
  { label: '近 24 小时', value: '24h' },
  { label: '近 7 天', value: '7d' },
]
const points = ref<TelemetryPoint[]>([])
const loading = ref(false)
const latestLoading = ref(false)
const selected = computed(() => latest.value.find(point => point.metric === metric.value) ?? null)
const values = computed(() => points.value.map(point => point.value))
const labels = computed(() => points.value.map(point => point.collectedAt.slice(11, 16)))

function fromTime(): string {
  const now = new Date()
  const hours = range.value === '1h' ? 1 : range.value === '6h' ? 6 : range.value === '24h' ? 24 : 24 * 7
  const start = new Date(now.getTime() - hours * 3600 * 1000)
  return start.toISOString().slice(0, 19).replace('T', ' ')
}
function toTime(): string {
  return new Date().toISOString().slice(0, 19).replace('T', ' ')
}

async function loadNodes() {
  try {
    const page = await deviceApi.nodes.list({ pageNum: 1, pageSize: 100 })
    nodes.value = page.rows
    if (!nodeId.value && page.rows.length) nodeId.value = page.rows[0].id
  } catch (error) {
    toastError(error, '节点加载失败')
  }
}

async function loadLatest() {
  if (!nodeId.value) return
  latestLoading.value = true
  try {
    latest.value = await fleetApi.telemetry.latest(nodeId.value)
    if (!metric.value || !latest.value.some(point => point.metric === metric.value)) {
      metric.value = latest.value[0]?.metric ?? ''
    }
  } catch (error) {
    toastError(error, '最新遥测加载失败')
  } finally {
    latestLoading.value = false
  }
}

async function loadSeries() {
  if (!nodeId.value || !metric.value) {
    points.value = []
    return
  }
  loading.value = true
  try {
    points.value = await fleetApi.telemetry.series(nodeId.value, metric.value, fromTime(), toTime())
  } catch (error) {
    toastError(error, '遥测曲线加载失败')
  } finally {
    loading.value = false
  }
}

watch(nodeId, async () => {
  metric.value = ''
  await loadLatest()
  await loadSeries()
})
watch(metric, loadSeries)
watch(range, loadSeries)

let timer = 0
onMounted(async () => {
  await loadNodes()
  await loadLatest()
  await loadSeries()
  timer = window.setInterval(() => { loadLatest(); loadSeries() }, 10000)
})
onBeforeUnmount(() => window.clearInterval(timer))
</script>

<template>
  <div class="page">
    <section class="band reveal">
      <div class="band-head">
        <SectionLabel title="遥测数据" />
        <div class="band-tools">
          <Select v-model="nodeId" :options="nodeOptions" placeholder="选择节点" filterable />
          <Select v-model="range" :options="ranges" />
        </div>
      </div>
      <div class="readout-strip">
        <Metric label="节点" :value="nodes.length" unit="个" />
        <Metric label="指标" :value="latest.length" unit="项" />
        <Metric label="采样点" :value="points.length" unit="个" />
      </div>
      <div class="metric-grid">
        <button
          v-for="point in latest"
          :key="point.metric"
          type="button"
          class="metric-card lift"
          :class="{ 'is-active': point.metric === metric }"
          @click="metric = point.metric"
        >
          <div class="metric-name soft">{{ point.metric }}</div>
          <div class="metric-value tnum" :title="`${point.value} ${point.unit}`">
            {{ formatMetricValue(point.value, point.unit) }}<span class="metric-unit soft">{{ point.unit }}</span>
          </div>
          <div class="metric-time mono faint">{{ point.collectedAt }}</div>
        </button>
        <EmptyState v-if="!latestLoading && !latest.length" title="暂无遥测" desc="Agent 上报后会显示指标。" icon="line-chart" />
      </div>
    </section>

    <section class="band reveal" style="margin-top:18px">
      <div class="band-head">
        <SectionLabel :title="selected ? `${selected.metric} 趋势` : '趋势'" />
        <Tag v-if="selected" tone="info">{{ selected.unit || '数值' }}</Tag>
      </div>
      <TrendChart v-if="values.length" :values="values" :labels="labels" :unit="selected?.unit || ''" :height="300" />
      <EmptyState v-else :title="loading ? '加载中' : '还没有曲线'" desc="选择节点与指标后显示时间序列。" icon="chart" />
    </section>
  </div>
</template>

<style scoped>
.metric-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(min(180px, 100%), 1fr));
  gap: 12px;
  margin-top: 16px;
}

.metric-card {
  min-width: 0;
  overflow: hidden;
  padding: 14px 16px;
  border: 1px solid var(--line);
  border-radius: 12px;
  background: var(--surface);
  color: inherit;
  font: inherit;
  text-align: left;
  cursor: pointer;
}

.metric-name,
.metric-time {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.metric-name { font-size: 12px; }
.metric-time { font-size: 11px; }

.metric-value {
  overflow: hidden;
  font-size: 24px;
  font-weight: 650;
  line-height: 1.25;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.metric-unit {
  margin-left: 2px;
  font-size: 12px;
}
</style>
