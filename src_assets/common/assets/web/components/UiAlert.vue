<template>
  <div :class="wrapper">
    <div class="flex items-start gap-3">
      <div v-if="icon" class="text-xl leading-none pt-0.5">
        <slot name="icon">
          {{ icon }}
        </slot>
      </div>
      <div class="flex-1">
        <slot />
      </div>
      <button
        v-if="dismissible"
        class="ml-2 text-solar-dark dark:text-lunar-light hover:opacity-70"
        @click="$emit('close')"
      >
        &times;
      </button>
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue';
const props = defineProps({
  variant: { type: String, default: 'info' },
  dismissible: { type: Boolean, default: false },
  icon: { type: String, default: '' },
});

const base = 'rounded-md px-4 py-3 text-sm shadow-sm border';
const variants = {
  danger:
    'bg-solar-danger/10 border-solar-danger/30 text-solar-danger dark:bg-lunar-danger/15 dark:border-lunar-danger/40 dark:text-lunar-danger',
  success:
    'bg-solar-success/10 border-solar-success/30 text-solar-success dark:bg-lunar-success/15 dark:border-lunar-success/40 dark:text-lunar-success',
  warning:
    'bg-solar-warning/10 border-solar-warning/30 text-solar-warning dark:bg-lunar-warning/15 dark:border-lunar-warning/40 dark:text-lunar-warning',
  info: 'bg-solar-info/10 border-solar-info/30 text-solar-info dark:bg-lunar-info/15 dark:border-lunar-info/40 dark:text-lunar-info',
  primary:
    'bg-solar-primary/10 border-solar-primary/30 text-solar-primary dark:bg-lunar-primary/15 dark:border-lunar-primary/40 dark:text-lunar-primary',
  neutral:
    'bg-solar-surface border-solar-dark/10 text-solar-dark dark:bg-lunar-surface dark:border-lunar-light/10 dark:text-lunar-light',
};
const wrapper = computed(() => `${base} ${variants[props.variant] || variants.info}`);
</script>

<style scoped></style>
