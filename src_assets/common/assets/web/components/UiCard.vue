<template>
  <div :class="outer">
    <div v-if="title || $slots.title" class="mb-4 flex items-center gap-3">
      <h2 v-if="title" class="text-2xl font-semibold tracking-tight">
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
    'rounded-xl bg-light dark:bg-surface text-dark dark:text-light border border-dark/10 dark:border-light/10',
    props.elevated ? 'shadow-md shadow-dark/12 dark:shadow-dark/30' : '',
    props.padded ? 'p-7' : '',
  ].join(' '),
);
</script>
<style scoped></style>
