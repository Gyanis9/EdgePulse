<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { deptTree, ensureDepts, flattenDepts } from '@/composables/dept'

/* 自定义数据范围的部门多选：按组织层级缩进，停用部门不可选。 */
const props = defineProps<{
  modelValue: number[]
  invalid?: boolean
}>()
const emit = defineEmits<{ (e: 'update:modelValue', value: number[]): void }>()

onMounted(() => ensureDepts())

const rows = computed(() => flattenDepts(deptTree.value))

function toggle(id: number) {
  const next = new Set(props.modelValue)
  if (next.has(id)) next.delete(id)
  else next.add(id)
  emit('update:modelValue', [...next])
}
</script>

<template>
  <div :class="['dept-multi', invalid && 'is-invalid']" role="group" aria-label="自定义部门">
    <label v-for="row in rows" :key="row.dept.id" class="dept-multi-row" :style="{ paddingLeft: `${8 + row.depth * 16}px` }">
      <input
        type="checkbox"
        :checked="modelValue.includes(row.dept.id)"
        :disabled="row.dept.status !== 1"
        @change="toggle(row.dept.id)"
      />
      <span>{{ row.dept.name }}</span>
      <span v-if="row.dept.status !== 1" class="faint">停用</span>
    </label>
    <p v-if="!rows.length" class="faint">暂无部门</p>
  </div>
</template>

<style scoped>
.dept-multi {
  max-height: 220px;
  overflow: auto;
  padding: 6px;
  border: 1px solid var(--line-strong);
  border-radius: 10px;
  background: var(--surface-solid);
}
.dept-multi.is-invalid { border-color: var(--red); }
.dept-multi-row {
  display: flex;
  gap: 8px;
  align-items: center;
  padding: 5px 8px;
  font-size: 13px;
  border-radius: 7px;
  cursor: pointer;
}
.dept-multi-row:hover { background: var(--line); }
.dept-multi-row input { accent-color: var(--red); }
</style>
