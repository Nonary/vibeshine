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
    <label for="max_bitrate" class="form-label">{{ $t('config.max_bitrate') }}</label>
    <input id="max_bitrate" v-model="config.max_bitrate" type="number" class="form-control" placeholder="0" />
    <p class="text-[11px] opacity-60 mt-1">
      {{ $t('config.max_bitrate_desc') }}
    </p>
  </div>

  <!--minimum_fps_target-->
  <div class="mb-4">
    <label for="minimum_fps_target" class="form-label">{{ $t('config.minimum_fps_target') }}</label>
    <input id="minimum_fps_target" v-model="config.minimum_fps_target" type="number" min="0" max="1000"
      class="form-control" placeholder="0" />
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
