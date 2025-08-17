<template>
  <button
    v-if="visible"
    type="button"
    class="flex items-center gap-2 text-xs select-none rounded-md px-2 py-1"
    :class="{ 'cursor-pointer hover:bg-black/5 dark:hover:bg-white/10 transition': canSave }"
    :title="tooltip"
    @click="onClick"
  >
    <i :class="iconClass" />
    <span class="opacity-80">{{ label }}</span>
  </button>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useRoute } from 'vue-router';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';

const route = useRoute();
const store = useConfigStore();
const { savingState, manualDirty } = storeToRefs(store);

const visible = computed(() => route.path === '/settings');
const canSave = computed(
  () => visible.value && (savingState.value === 'error' || manualDirty.value === true),
);

const label = computed(() => {
  switch (savingState.value) {
    case 'saving':
      return 'Save Status: Saving…';
    case 'dirty':
      return manualDirty.value
        ? 'Save Status: Unsaved Changes (Click to Save)'
        : 'Save Status: Unsaved Changes';
    case 'saved':
      return 'Save Status: Saved';
    case 'error':
      return 'Save Status: Error (Tap to Retry)';
    default:
      return 'Save Status: Waiting for Changes';
  }
});

const iconClass = computed(() => {
  const base = 'fas text-[11px]';
  switch (savingState.value) {
    case 'saving':
      return base + ' fa-spinner animate-spin opacity-80';
    case 'dirty':
      return base + ' fa-circle-exclamation text-amber-600 dark:text-amber-400';
    case 'saved':
      return base + ' fa-check text-green-600 dark:text-green-400';
    case 'error':
      return base + ' fa-triangle-exclamation text-red-600 dark:text-red-400';
    default:
      return base + ' fa-circle opacity-60 pulse-soft';
  }
});

const tooltip = computed(
  () => 'This page auto-saves most changes as you edit. Some fields may require clicking Save.',
);

async function onClick() {
  if (!canSave.value) return;
  try {
    await store.save();
  } catch {}
}
</script>

<style scoped>
@keyframes pulseSoft {
  0%,
  100% {
    opacity: 0.55;
    transform: scale(1);
  }
  50% {
    opacity: 0.9;
    transform: scale(1.06);
  }
}
.pulse-soft {
  animation: pulseSoft 1.6s ease-in-out infinite;
}
</style>
