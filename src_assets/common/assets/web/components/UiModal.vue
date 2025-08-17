<template>
  <transition name="fade-fast">
    <div v-if="open" class="fixed inset-0 z-[120] flex flex-col">
      <div
        class="absolute inset-0 bg-gradient-to-br from-black/70 via-black/60 to-black/70 backdrop-blur-md"
        @click="backdropClose ? emit('update:open', false) : null"
      ></div>
      <div class="relative flex-1 flex flex-col items-center justify-center p-4 overflow-y-auto">
        <div
          class="relative w-full max-w-lg rounded-2xl shadow-2xl border border-white/10 dark:border-white/10 bg-white/90 dark:bg-neutral-900/85 backdrop-blur-xl ring-1 ring-black/5 dark:ring-white/5 flex flex-col"
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
            <h3 v-if="title" class="text-xl font-semibold text-dark dark:text-light">
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

<script setup>
import { useSlots } from 'vue';
defineProps({
  open: { type: Boolean, default: false },
  title: { type: String, default: '' },
  dismissible: { type: Boolean, default: true },
  backdropClose: { type: Boolean, default: true },
});
const emit = defineEmits(['update:open']);
const slots = useSlots();
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
