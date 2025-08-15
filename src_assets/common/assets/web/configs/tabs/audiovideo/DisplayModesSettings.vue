<script setup>
import { ref } from 'vue';
import { $tp } from '@/platform-i18n';
import PlatformLayout from '@/PlatformLayout.vue';
import { useConfigStore } from '@/stores/config.js';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <!--max_bitrate-->
  <div class="mb-4">
    <label
      for="max_bitrate"
      class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light"
      >{{ $t('config.max_bitrate') }}</label
    >
    <input
      id="max_bitrate"
      v-model="config.max_bitrate"
      type="number"
      class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15"
      placeholder="0"
    />
    <p class="text-[11px] opacity-60 mt-1">
      {{ $t('config.max_bitrate_desc') }}
    </p>
  </div>

  <!--minimum_fps_target-->
  <div class="mb-4">
    <label
      for="minimum_fps_target"
      class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light"
      >{{ $t('config.minimum_fps_target') }}</label
    >
    <input
      id="minimum_fps_target"
      v-model="config.minimum_fps_target"
      type="number"
      min="0"
      max="1000"
      class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15"
      placeholder="0"
    />
    <p class="text-[11px] opacity-60 mt-1">
      {{ $t('config.minimum_fps_target_desc') }}
    </p>
  </div>
</template>

<style scoped>
.ms-item {
  background-color: var(--bs-dark-bg-subtle);
  font-size: 12px;
  font-weight: bold;
}
</style>
