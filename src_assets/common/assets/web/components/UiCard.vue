<template>
  <div :class="outer">
    <div v-if="title || $slots.title" class="mb-3 flex items-center gap-2">
      <h2 class="text-lg font-semibold tracking-tight" v-if="title">{{ title }}</h2>
      <slot name="title" />
      <div class="ml-auto" v-if="$slots.actions"><slot name="actions" /></div>
    </div>
    <slot />
    <div v-if="$slots.footer" class="mt-4 pt-3 border-t border-solar-dark/10 dark:border-lunar-light/10"><slot name="footer" /></div>
  </div>
</template>
<script setup>
import { computed } from 'vue'
const props = defineProps({
  elevated: { type: Boolean, default: true },
  padded: { type: Boolean, default: true },
  title: { type: String, default: '' }
})
const outer = computed(() => [
  'rounded-xl bg-solar-light dark:bg-lunar-surface text-solar-dark dark:text-lunar-light border border-solar-dark/10 dark:border-lunar-light/10',
  props.elevated ? 'shadow-sm shadow-solar-dark/10 dark:shadow-lunar-dark/20' : '',
  props.padded ? 'p-5' : ''
].join(' '))
</script>
<style scoped></style>
