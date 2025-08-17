<template>
  <UiModal :open="open" :title="title" :dismissible="dismissible" @update:open="onOpen">
    <div class="space-y-3">
      <div v-if="icon" class="text-2xl"><i :class="icon" /></div>
      <div class="text-sm leading-relaxed text-dark dark:text-light">
        <slot>
          {{ message }}
        </slot>
      </div>
    </div>
    <template #footer>
      <div class="flex items-center justify-end gap-2">
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
import UiModal from '@/components/UiModal.vue';
import UiButton from '@/components/UiButton.vue';
defineProps({
  open: { type: Boolean, default: false },
  title: { type: String, default: '' },
  message: { type: String, default: '' },
  cancelText: { type: String, default: 'Cancel' },
  confirmText: { type: String, default: 'Confirm' },
  confirmIcon: { type: String, default: '' },
  variant: { type: String, default: 'danger' },
  dismissible: { type: Boolean, default: true },
  icon: { type: String, default: '' },
});
const emit = defineEmits(['update:open', 'confirm', 'cancel']);
function onOpen(v) {
  emit('update:open', v);
}
</script>

<style scoped></style>
