<template>
  <transition name="fade-fast">
    <div
      v-if="open"
      class="fixed inset-0 z-[120] flex flex-col"
      role="dialog"
      aria-modal="true"
      :aria-labelledby="title ? 'ui-modal-title' : undefined"
    >
      <div
        class="absolute inset-0 bg-gradient-to-br from-black/70 via-black/60 to-black/70 backdrop-blur-md"
        @click="backdropClose ? emit('update:open', false) : null"
      ></div>
      <div class="relative flex-1 flex flex-col items-center justify-center px-4 py-12 overflow-y-auto">
        <div
          ref="panelEl"
          :class="['relative w-full', maxWidthClass, 'rounded-2xl shadow-2xl border border-white/10 dark:border-white/10 bg-white/90 dark:bg-neutral-900/85 backdrop-blur-xl ring-1 ring-black/5 dark:ring-white/5 flex flex-col']"
        >
          <!-- Close button (absolute, non-intrusive) -->
          <button
            v-if="dismissible"
            class="absolute top-3 right-3 p-2 rounded-md text-dark/70 dark:text-light/80 hover:bg-black/5 dark:hover:bg-white/10 focus:outline-none focus-visible:ring-2 focus-visible:ring-primary/50"
            aria-label="Close"
            @click="emit('update:open', false)"
          >
            <i class="fas fa-times" />
          </button>
          <!-- Icon row -->
          <div v-if="slots.icon" class="px-6 pt-8 flex items-center justify-center">
            <slot name="icon" />
          </div>

          <!-- Title row -->
          <header class="px-8 pt-3 pb-1 flex items-center justify-center text-center">
            <h3 v-if="title" id="ui-modal-title" class="text-xl font-semibold text-dark dark:text-light">
              {{ title }}
            </h3>
            <slot name="title" />
          </header>

          <!-- Body row -->
          <div class="px-8 py-5 flex-1">
            <slot />
          </div>

          <!-- Footer row (dedicated for buttons) -->
          <footer v-if="slots.footer" class="px-8 pb-6 flex items-center justify-end gap-3">
            <slot name="footer" />
          </footer>
        </div>
      </div>
    </div>
  </transition>
</template>

<script setup lang="ts">
import { useSlots, computed, onMounted, onBeforeUnmount, ref, nextTick } from 'vue';
const props = defineProps({
  open: { type: Boolean, default: false },
  title: { type: String, default: '' },
  dismissible: { type: Boolean, default: true },
  backdropClose: { type: Boolean, default: true },
  size: { type: String, default: 'lg' }, // lg|xl|2xl|3xl
});
const emit = defineEmits(['update:open']);
const slots = useSlots();
const panelEl = ref(null as null | HTMLElement);

const maxWidthClass = computed(() => {
  switch (props.size) {
    case '3xl':
      return 'max-w-6xl';
    case '2xl':
      return 'max-w-4xl';
    case 'xl':
      return 'max-w-3xl';
    default:
      return 'max-w-lg';
  }
});

function onKeydown(e: KeyboardEvent) {
  if (!props.open) return;
  if (e.key === 'Escape' && props.dismissible) {
    emit('update:open', false);
  }
  // Basic focus trap
  if (e.key === 'Tab' && panelEl.value) {
    const focusables = panelEl.value.querySelectorAll<HTMLElement>(
      'a[href], button:not([disabled]), textarea, input, select, [tabindex]:not([tabindex="-1"])',
    );
    const list = Array.from(focusables).filter((n) => !n.hasAttribute('disabled'));
    if (list.length === 0) return;
    const first = list[0];
    const last = list[list.length - 1];
    const active = document.activeElement as HTMLElement | null;
    if (e.shiftKey) {
      if (active === first || !panelEl.value.contains(active)) {
        e.preventDefault();
        last.focus();
      }
    } else {
      if (active === last) {
        e.preventDefault();
        first.focus();
      }
    }
  }
}

onMounted(async () => {
  document.addEventListener('keydown', onKeydown);
  await nextTick();
  // focus first focusable inside the panel
  if (panelEl.value) {
    const first = panelEl.value.querySelector<HTMLElement>(
      'input, textarea, button, select, [tabindex]:not([tabindex="-1"])',
    );
    first?.focus?.();
  }
});
onBeforeUnmount(() => document.removeEventListener('keydown', onKeydown));
</script>

<style scoped>
.fade-fast-enter-active,
.fade-fast-leave-active {
  transition: opacity 0.15s;
}
.fade-fast-enter-from,
.fade-fast-leave-to {
  opacity: 0;
}
</style>
