<template>
  <component
    :is="asTag"
    :type="asTag === 'button' ? type : undefined"
    :href="asTag !== 'button' ? href : undefined"
    :target="target"
    :rel="rel"
    :class="computedClasses"
    v-bind="$attrs"
    :disabled="asTag === 'button' ? disabled || loading : undefined"
  >
    <slot />
  </component>
</template>

<script setup>
import { computed } from 'vue';

const props = defineProps({
  variant: { type: String, default: 'primary' },
  tone: { type: String, default: 'solid' }, // solid | outline | ghost
  size: { type: String, default: 'md' }, // sm | md | lg
  type: { type: String, default: 'button' },
  block: { type: Boolean, default: false },
  loading: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
  as: { type: String, default: 'button' }, // button | a
  href: { type: String, default: '' },
  target: { type: String, default: undefined },
  rel: { type: String, default: undefined },
});

const asTag = computed(() => (props.as === 'a' ? 'a' : 'button'));

const base =
  'relative inline-flex items-center justify-center gap-2 whitespace-nowrap font-medium rounded-md transition leading-none focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-offset-2 disabled:opacity-50 disabled:cursor-not-allowed';

const variantMap = {
  primary: 'bg-primary text-onPrimary',
  success: 'bg-success text-onPrimary',
  danger: 'bg-danger text-onPrimary',
  warning: 'bg-warning text-onPrimary',
  secondary: 'bg-secondary text-onSecondary',
  info: 'bg-info text-onPrimary',
  neutral: 'bg-surface text-onLight dark:text-onDark',
};

const outlineMap = {
  primary: 'border border-primary text-primary',
  success: 'border border-success text-success',
  danger: 'border border-danger text-danger',
  warning: 'border border-warning text-warning',
  secondary: 'border border-secondary text-secondary',
  info: 'border border-info text-info',
  neutral: 'border border-dark text-dark dark:border-light/40 dark:text-light',
};

const ghostMap = {
  primary: 'text-primary hover:bg-primary/10',
  success: 'text-success hover:bg-success/10',
  danger: 'text-danger hover:bg-danger/10',
  warning: 'text-warning hover:bg-warning/10',
  secondary: 'text-secondary hover:bg-secondary/10',
  info: 'text-info hover:bg-info/10',
  neutral: 'text-dark dark:text-light hover:bg-dark/5 dark:hover:bg-light/10',
};

const sizeMap = {
  sm: 'text-xs px-2 py-1',
  md: 'text-sm px-3 py-2',
  lg: 'text-base px-4 py-2.5',
};

const computedClasses = computed(() => {
  let palette =
    props.tone === 'outline' ? outlineMap : props.tone === 'ghost' ? ghostMap : variantMap;
  const toneHover =
    props.tone === 'solid'
      ? 'hover:brightness-[1.08] active:brightness-95'
      : props.tone === 'outline'
        ? 'hover:bg-primary/5'
        : '';
  return [
    base,
    sizeMap[props.size],
    palette[props.variant] || palette.primary,
    toneHover,
    props.block ? 'w-full' : '',
    props.loading ? 'cursor-wait' : '',
  ].join(' ');
});
</script>

<style scoped>
/* Normalize icon sizing and alignment inside buttons */
:deep(i),
:deep(svg) {
  line-height: 1;
  display: inline-block;
}
</style>
