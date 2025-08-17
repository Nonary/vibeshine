<template>
  <UiModal :open="open" :title="title" :dismissible="dismissible" @update:open="onOpen">
    <template #icon>
      <div v-if="icon" :class="['mx-auto', iconSizeClass, iconColorClass]">
        <i :class="icon" />
      </div>
    </template>
    <div class="text-sm leading-relaxed text-dark dark:text-light text-center px-2">
      <slot>
        {{ message }}
      </slot>
    </div>
    <template #footer>
      <div class="w-full flex items-center justify-center gap-3">
        <UiButton tone="ghost" variant="neutral" @click="emit('cancel')">{{ cancelText }}</UiButton>
        <UiButton :variant="variant" @click="emit('confirm')">
          <i v-if="confirmIcon" :class="confirmIcon" />
          {{ confirmText }}
        </UiButton>
      </div>
    </template>
  </UiModal>
</template>

<script setup>
import { computed } from 'vue';
import UiModal from '@/components/UiModal.vue';
import UiButton from '@/components/UiButton.vue';
const props = defineProps({
  open: { type: Boolean, default: false },
  title: { type: String, default: '' },
  message: { type: String, default: '' },
  cancelText: { type: String, default: 'Cancel' },
  confirmText: { type: String, default: 'Delete' },
  confirmIcon: { type: String, default: '' },
  variant: { type: String, default: 'danger' },
  dismissible: { type: Boolean, default: true },
  icon: { type: String, default: '' },
});
const emit = defineEmits(['update:open', 'confirm', 'cancel']);
function onOpen(v) {
  emit('update:open', v);
}
// Presentation helpers
const iconSizeClass = 'text-5xl';
const iconColorClass = computed(() => {
  switch (props.variant) {
    case 'danger':
      return 'text-red-600 dark:text-red-400';
    case 'success':
      return 'text-emerald-600 dark:text-emerald-400';
    case 'warning':
      return 'text-amber-600 dark:text-amber-400';
    default:
      return 'text-primary';
  }
});
</script>

<style scoped></style>
