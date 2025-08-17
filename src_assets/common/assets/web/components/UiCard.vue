<template>
  <div :class="outer">
    <div v-if="title || $slots.title" class="card-header">
      <h2 v-if="title" class="text-lg font-medium flex items-center gap-2">
        {{ title }}
      </h2>
      <slot name="title" />
      <div v-if="$slots.actions" class="ml-auto">
        <slot name="actions" />
      </div>
    </div>
    <slot />
    <div v-if="$slots.footer" class="mt-4 pt-3 border-t border-dark/10 dark:border-light/10">
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
    // Match client management card style closely
    'rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface text-dark dark:text-light shadow-sm',
    props.padded ? 'p-5' : '',
    props.elevated ? '' : '',
  ].join(' '),
);
</script>
<style scoped></style>
