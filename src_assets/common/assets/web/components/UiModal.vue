<template>
  <transition name="fade-fast">
    <div v-if="open" class="fixed inset-0 z-[120] flex flex-col">
      <div
        class="absolute inset-0 bg-gradient-to-br from-black/70 via-black/60 to-black/70 backdrop-blur-md"
        @click="backdropClose ? emit('update:open', false) : null"
      ></div>
      <div class="relative flex-1 flex flex-col items-center justify-center p-4 overflow-y-auto">
        <div
          class="w-full max-w-lg rounded-2xl shadow-2xl border border-white/10 dark:border-white/10 bg-white/90 dark:bg-neutral-900/85 backdrop-blur-xl ring-1 ring-black/5 dark:ring-white/5"
        >
          <header
            class="px-6 py-5 border-b border-black/10 dark:border-white/10 flex items-center gap-3"
          >
            <h3 v-if="title" class="text-lg font-semibold text-dark dark:text-light">
              {{ title }}
            </h3>
            <slot name="title" />
            <button
              v-if="dismissible"
              class="ml-auto text-dark/70 dark:text-light/80 hover:opacity-80"
              aria-label="Close"
              @click="emit('update:open', false)"
            >
              <i class="fas fa-times" />
            </button>
          </header>
          <div class="px-6 py-5">
            <slot />
          </div>
          <footer
            v-if="$slots.footer"
            class="px-6 py-4 border-t border-black/10 dark:border-white/10"
          >
            <slot name="footer" />
          </footer>
        </div>
      </div>
    </div>
  </transition>
</template>

<script setup>
defineProps({
  open: { type: Boolean, default: false },
  title: { type: String, default: '' },
  dismissible: { type: Boolean, default: true },
  backdropClose: { type: Boolean, default: true },
});
const emit = defineEmits(['update:open']);
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
