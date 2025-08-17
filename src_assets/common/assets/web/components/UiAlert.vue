<template>
  <div :class="wrapper" role="alert" aria-live="polite">
    <div class="flex items-start gap-3">
      <div class="shrink-0 mt-0.5">
        <div :class="iconWrapClass">
          <slot name="icon">
            <i :class="iconClass" />
          </slot>
        </div>
      </div>
      <div class="flex-1 leading-relaxed">
        <slot />
      </div>
      <button
        v-if="dismissible"
        class="ml-2 opacity-70 hover:opacity-100 transition text-current"
        aria-label="Close"
        @click="$emit('close')"
      >
        <i class="fas fa-xmark" />
      </button>
    </div>
  </div>
  
</template>

<script setup>
import { computed } from 'vue';
const props = defineProps({
  variant: { type: String, default: 'info' },
  dismissible: { type: Boolean, default: false },
  icon: { type: String, default: '' }, // legacy; slot overrides; default icons used when empty
});
defineEmits(['close']);

const base = 'rounded-md px-4 py-3 text-sm shadow-sm border';
const variants = {
  danger: 'bg-danger/15 border-danger/40 text-danger',
  success: 'bg-success/15 border-success/40 text-success',
  warning: 'bg-warning/15 border-warning/40 text-warning',
  info: 'bg-info/15 border-info/40 text-info',
  primary: 'bg-primary/15 border-primary/40 text-primary',
  neutral: 'bg-surface border-dark/10 text-dark dark:text-light',
};
const wrapper = computed(() => `${base} ${variants[props.variant] || variants.info}`);

const iconWrap = {
  danger: 'bg-danger/20 text-danger',
  success: 'bg-success/20 text-success',
  warning: 'bg-warning/20 text-warning',
  info: 'bg-info/20 text-info',
  primary: 'bg-primary/20 text-primary',
  neutral: 'bg-dark/10 text-dark dark:text-light',
};
const iconWrapClass = computed(
  () => `inline-flex items-center justify-center rounded-full w-7 h-7 ${iconWrap[props.variant] || iconWrap.info}`,
);

const defaultIcons = {
  danger: 'fas fa-circle-exclamation',
  success: 'fas fa-circle-check',
  warning: 'fas fa-triangle-exclamation',
  info: 'fas fa-circle-info',
  primary: 'fas fa-bell',
  neutral: 'fas fa-info',
};
const iconClass = computed(() => props.icon || defaultIcons[props.variant] || defaultIcons.info);
</script>

<style scoped></style>
