<script setup lang="ts">
import { computed } from 'vue'
import { ICON_PATHS, type IconDefinition, type IconNode } from '@/data/icons'

const props = withDefaults(defineProps<{
  name: string
  size?: number
  stroke?: number
}>(), {
  size: 18,
  stroke: 1.6,
})

const definition = computed<IconDefinition>(() => ICON_PATHS[props.name] ?? ICON_PATHS.file)
const nodes = computed<readonly IconNode[] | null>(() => Array.isArray(definition.value) ? definition.value : null)
</script>

<template>
  <svg
    class="icon"
    :width="size"
    :height="size"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    :stroke-width="stroke"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
    style="flex: none"
  >
    <path v-if="typeof definition === 'string'" :d="definition" />
    <template v-else-if="nodes">
      <template v-for="(node, index) in nodes" :key="index">
        <path v-if="node[0] === 'path'" v-bind="node[1]" />
        <rect v-else-if="node[0] === 'rect'" v-bind="node[1]" />
        <circle v-else-if="node[0] === 'circle'" v-bind="node[1]" />
        <ellipse v-else-if="node[0] === 'ellipse'" v-bind="node[1]" />
        <line v-else-if="node[0] === 'line'" v-bind="node[1]" />
        <polyline v-else-if="node[0] === 'polyline'" v-bind="node[1]" />
        <polygon v-else-if="node[0] === 'polygon'" v-bind="node[1]" />
      </template>
    </template>
  </svg>
</template>
