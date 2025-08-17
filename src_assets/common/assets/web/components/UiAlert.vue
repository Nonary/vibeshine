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
        class="ml-2 text-dark dark:text-light hover:opacity-70"
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
defineEmits(['close']);

const base = 'rounded-md px-4 py-3 text-sm shadow-sm border';
const variants = {
  danger: 'bg-danger/10 border-danger/30 text-danger',
  success: 'bg-success/10 border-success/30 text-success',
  warning: 'bg-warning/10 border-warning/30 text-warning',
  info: 'bg-info/10 border-info/30 text-info',
  primary: 'bg-primary/10 border-primary/30 text-primary',
  neutral: 'bg-surface border-dark/10 text-dark dark:text-light',
};
const wrapper = computed(() => `${base} ${variants[props.variant] || variants.info}`);
</script>

<style scoped></style>
