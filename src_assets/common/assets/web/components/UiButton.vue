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
  'relative inline-flex items-center justify-center font-medium rounded-md transition focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-offset-2 disabled:opacity-50 disabled:cursor-not-allowed';

const variantMap = {
  primary: 'bg-solar-primary text-solar-onPrimary dark:bg-lunar-primary dark:text-lunar-onPrimary',
  success: 'bg-solar-success text-solar-onPrimary dark:bg-lunar-success dark:text-lunar-onPrimary',
  danger: 'bg-solar-danger text-solar-onPrimary dark:bg-lunar-danger dark:text-lunar-onPrimary',
  warning: 'bg-solar-warning text-solar-onPrimary dark:bg-lunar-warning dark:text-lunar-onPrimary',
  secondary:
    'bg-solar-secondary text-solar-onSecondary dark:bg-lunar-secondary dark:text-lunar-onSecondary',
  info: 'bg-solar-info text-solar-onPrimary dark:bg-lunar-info dark:text-lunar-onPrimary',
  neutral: 'bg-solar-surface text-solar-onLight dark:bg-lunar-surface dark:text-lunar-onLight',
};

const outlineMap = {
  primary:
    'border border-solar-primary text-solar-primary dark:border-lunar-primary dark:text-lunar-primary',
  success:
    'border border-solar-success text-solar-success dark:border-lunar-success dark:text-lunar-success',
  danger:
    'border border-solar-danger text-solar-danger dark:border-lunar-danger dark:text-lunar-danger',
  warning:
    'border border-solar-warning text-solar-warning dark:border-lunar-warning dark:text-lunar-warning',
  secondary:
    'border border-solar-secondary text-solar-secondary dark:border-lunar-secondary dark:text-lunar-secondary',
  info: 'border border-solar-info text-solar-info dark:border-lunar-info dark:text-lunar-info',
  neutral: 'border border-solar-dark text-solar-dark dark:border-lunar-light dark:text-lunar-light',
};

const ghostMap = {
  primary:
    'text-solar-primary dark:text-lunar-primary hover:bg-solar-primary/10 dark:hover:bg-lunar-primary/10',
  success:
    'text-solar-success dark:text-lunar-success hover:bg-solar-success/10 dark:hover:bg-lunar-success/10',
  danger:
    'text-solar-danger dark:text-lunar-danger hover:bg-solar-danger/10 dark:hover:bg-lunar-danger/10',
  warning:
    'text-solar-warning dark:text-lunar-warning hover:bg-solar-warning/10 dark:hover:bg-lunar-warning/10',
  secondary:
    'text-solar-secondary dark:text-lunar-secondary hover:bg-solar-secondary/10 dark:hover:bg-lunar-secondary/10',
  info: 'text-solar-info dark:text-lunar-info hover:bg-solar-info/10 dark:hover:bg-lunar-info/10',
  neutral:
    'text-solar-dark dark:text-lunar-light hover:bg-solar-dark/5 dark:hover:bg-lunar-light/5',
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
        ? 'hover:bg-solar-primary/5 dark:hover:bg-lunar-primary/5'
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

<style scoped></style>
