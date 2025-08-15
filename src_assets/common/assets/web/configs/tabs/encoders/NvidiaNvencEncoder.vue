<script setup>
import { ref } from 'vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config.js';
import { computed } from 'vue';

const store = useConfigStore();
const config = store.config;
const platform = computed(() => config.value?.platform || '');
</script>

<template>
  <div id="nvidia-nvenc-encoder" class="config-page">
    <!-- Performance preset -->
    <div class="mb-4">
      <label for="nvenc_preset" class="form-label">{{ $t('config.nvenc_preset') }}</label>
      <select id="nvenc_preset" v-model="config.nvenc_preset" class="form-control">
        <option value="1">P1 {{ $t('config.nvenc_preset_1') }}</option>
        <option value="2">P2</option>
        <option value="3">P3</option>
        <option value="4">P4</option>
        <option value="5">P5</option>
        <option value="6">P6</option>
        <option value="7">P7 {{ $t('config.nvenc_preset_7') }}</option>
      </select>
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.nvenc_preset_desc') }}
      </p>
    </div>

    <!-- Two-pass mode -->
    <div class="mb-4">
      <label for="nvenc_twopass" class="form-label">{{ $t('config.nvenc_twopass') }}</label>
      <select id="nvenc_twopass" v-model="config.nvenc_twopass" class="form-control">
        <option value="disabled">
          {{ $t('config.nvenc_twopass_disabled') }}
        </option>
        <option value="quarter_res">
          {{ $t('config.nvenc_twopass_quarter_res') }}
        </option>
        <option value="full_res">
          {{ $t('config.nvenc_twopass_full_res') }}
        </option>
      </select>
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.nvenc_twopass_desc') }}
      </p>
    </div>

    <!-- Spatial AQ -->
    <Checkbox id="nvenc_spatial_aq" v-model="config.nvenc_spatial_aq" class="mb-3" locale-prefix="config"
      default="false" />

    <!-- Single-frame VBV/HRD percentage increase -->
    <div class="mb-4">
      <label for="nvenc_vbv_increase" class="form-label">{{ $t('config.nvenc_vbv_increase') }}</label>
      <input id="nvenc_vbv_increase" v-model="config.nvenc_vbv_increase" type="number" min="0" max="400"
        class="form-control" placeholder="0" />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.nvenc_vbv_increase_desc') }}<br />
        <br />
        <a href="https://en.wikipedia.org/wiki/Video_buffering_verifier">VBV/HRD</a>
      </p>
    </div>

    <!-- Miscellaneous options -->
    <div class="mb-4 rounded-md overflow-hidden border border-black/5 dark:border-white/10">
      <div class="bg-surface/40 dark:bg-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">
          {{ $t('config.misc') }}
        </h3>
      </div>
      <div class="p-4">
        <!-- NVENC Realtime HAGS priority -->
        <Checkbox v-if="platform === 'windows'" id="nvenc_realtime_hags" v-model="config.nvenc_realtime_hags"
          class="mb-3" locale-prefix="config" default="true">
          <br />
          <br />
          <a href="https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/">HAGS</a>
        </Checkbox>

        <!-- Prefer lower encoding latency over power savings -->
        <Checkbox v-if="platform === 'windows'" id="nvenc_latency_over_power" v-model="config.nvenc_latency_over_power"
          class="mb-3" locale-prefix="config" default="true" />

        <!-- Present OpenGL/Vulkan on top of DXGI -->
        <Checkbox v-if="platform === 'windows'" id="nvenc_opengl_vulkan_on_dxgi"
          v-model="config.nvenc_opengl_vulkan_on_dxgi" class="mb-3" locale-prefix="config" default="true" />

        <!-- NVENC H264 CAVLC -->
        <Checkbox id="nvenc_h264_cavlc" v-model="config.nvenc_h264_cavlc" class="mb-3" locale-prefix="config"
          default="false" />
      </div>
    </div>
  </div>
</template>

<style scoped></style>
