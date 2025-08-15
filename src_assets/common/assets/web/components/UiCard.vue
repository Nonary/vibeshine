<template>
  <div :class="outer">
    <div v-if="title || $slots.title" class="mb-3 flex items-center gap-2">
      <h2 v-if="title" class="text-lg font-semibold tracking-tight">
        {{ title }}
      </h2>
      <slot name="title" />
      <div v-if="$slots.actions" class="ml-auto">
        <slot name="actions" />
      </div>
    </div>
    <slot />
    <div v-if="$slots.footer" class="mt-4 pt-3 border-t border-dark/10">
      <slot name="footer" />
    </div>
  </div>
</template>
<script setup>
import { computed } from 'vue';
const props = defineProps({
  elevated: { type: Boolean, default: true },
  padded: { type: Boolean, default: true },
  title: { type: String, default: '' },
});
const outer = computed(() =>
  [
    'rounded-xl bg-light dark:bg-surface text-dark border border-dark/10',
    props.elevated ? 'shadow-sm shadow-dark/10 dark:shadow-dark/20' : '',
    props.padded ? 'p-5' : '',
  ].join(' '),
);
</script>
<style scoped></style>
