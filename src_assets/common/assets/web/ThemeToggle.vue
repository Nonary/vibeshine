<script setup>
import { loadAutoTheme, setupThemeToggleListener } from '@/theme';
import { onMounted } from 'vue';

onMounted(() => {
  loadAutoTheme();
  setupThemeToggleListener();
});
</script>

<template>
  <div class="relative inline-block text-left">
    <button
      id="bd-theme"
      aria-haspopup="true"
      :aria-expanded="open"
      class="flex items-center gap-2 px-2 py-1 rounded-md hover:bg-black/5 dark:hover:bg-white/5"
      @click="open = !open"
    >
      <span class="theme-icon-active"><i :class="activeIcon" /></span>
      <span id="bd-theme-text">{{ $t('navbar.toggle_theme') }}</span>
    </button>

    <div
      v-if="open"
      class="origin-top-right absolute right-0 mt-2 w-44 rounded-md shadow-lg bg-white dark:bg-lunar-surface/90 ring-1 ring-black/5 dark:ring-white/5"
    >
      <div class="py-1">
        <button
          v-for="opt in options"
          :key="opt.value"
          :class="[
            'w-full text-left px-3 py-2 flex items-center gap-2',
            opt.value === current ? 'bg-black/5 dark:bg-white/5' : '',
          ]"
          @click="select(opt.value)"
        >
          <i :class="opt.icon" />
          <span>{{ opt.label }}</span>
        </button>
      </div>
    </div>
  </div>
</template>

<script>
export default {
  data() {
    return {
      open: false,
      current: null,
      options: [
        { value: 'light', label: this.$t('navbar.theme_light'), icon: 'fa-solid fa-sun' },
        { value: 'dark', label: this.$t('navbar.theme_dark'), icon: 'fa-solid fa-moon' },
        {
          value: 'auto',
          label: this.$t('navbar.theme_auto'),
          icon: 'fa-solid fa-circle-half-stroke',
        },
      ],
    };
  },
  computed: {
    activeIcon() {
      const opt = this.options.find((o) => o.value === this.current) || this.options[2];
      return opt.icon;
    },
  },
  mounted() {
    // initialize current theme
    import('@/theme').then((mod) => {
      this.current = mod.getPreferredTheme();
    });
  },
  methods: {
    select(v) {
      import('@/theme').then((mod) => {
        // Use exported helpers from theme.js
        mod.setStoredTheme(v);
        mod.setTheme(v);
        this.current = v;
        this.open = false;
      });
    },
  },
};
</script>

<style scoped></style>
