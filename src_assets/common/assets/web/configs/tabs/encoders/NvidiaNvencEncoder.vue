<script setup lang="ts">
import { computed } from 'vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
const platform = computed(() => config.platform || '');
</script>

<template>
  <div id="nvidia-nvenc-encoder" class="config-page">
    <header class="section-header">
      <h3 class="text-sm font-medium">
        {{ $t('config.nvenc_section_title') }}
      </h3>
      <p
        v-if="$t('config.nvenc_section_desc') !== 'config.nvenc_section_desc'"
        class="text-xs opacity-70 mt-1"
      >
        {{ $t('config.nvenc_section_desc') }}
      </p>
    </header>

    <ConfigFieldRenderer setting-key="nvenc_preset" v-model="config.nvenc_preset" class="mb-4" />

    <ConfigFieldRenderer setting-key="nvenc_twopass" v-model="config.nvenc_twopass" class="mb-4" />

    <ConfigFieldRenderer
      setting-key="nvenc_spatial_aq"
      v-model="config.nvenc_spatial_aq"
      class="mb-3"
    />

    <ConfigFieldRenderer
      setting-key="nvenc_split_encode"
      v-model="config.nvenc_split_encode"
      class="mb-4"
    />

    <ConfigFieldRenderer
      setting-key="nvenc_vbv_increase"
      v-model="config.nvenc_vbv_increase"
      class="mb-4"
    >
      <span class="mt-2 inline-flex flex-wrap items-center gap-1 text-[11px] opacity-80">
        <span>{{ $t('_common.see_more') }}</span>
        <a
          class="text-primary underline decoration-primary/40 underline-offset-2"
          href="https://en.wikipedia.org/wiki/Video_buffering_verifier"
          target="_blank"
          rel="noopener noreferrer"
        >
          VBV/HRD
        </a>
      </span>
    </ConfigFieldRenderer>

    <div class="mb-4 rounded-md overflow-hidden border border-dark/10 dark:border-light/10">
      <div class="bg-surface/40 dark:bg-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">
          {{ $t('config.misc') }}
        </h3>
      </div>
      <div class="p-4">
        <ConfigFieldRenderer
          v-if="platform === 'windows'"
          setting-key="nvenc_realtime_hags"
          v-model="config.nvenc_realtime_hags"
          class="mb-3"
        >
          <span class="mt-2 inline-flex flex-wrap items-center gap-1 text-[11px] opacity-80">
            <span>{{ $t('_common.see_more') }}</span>
            <a
              class="text-primary underline decoration-primary/40 underline-offset-2"
              href="https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/"
              target="_blank"
              rel="noopener noreferrer"
            >
              HAGS
            </a>
          </span>
        </ConfigFieldRenderer>

        <ConfigFieldRenderer
          v-if="platform === 'windows'"
          setting-key="nvenc_latency_over_power"
          v-model="config.nvenc_latency_over_power"
          class="mb-3"
        />

        <ConfigFieldRenderer
          v-if="platform === 'windows'"
          setting-key="nvenc_opengl_vulkan_on_dxgi"
          v-model="config.nvenc_opengl_vulkan_on_dxgi"
          class="mb-3"
        />

        <ConfigFieldRenderer
          setting-key="nvenc_h264_cavlc"
          v-model="config.nvenc_h264_cavlc"
          class="mb-3"
        />
      </div>
    </div>
  </div>
</template>

<style scoped>
.section-header {
  @apply mb-4;
}
</style>
