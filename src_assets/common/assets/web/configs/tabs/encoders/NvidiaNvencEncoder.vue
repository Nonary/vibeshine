<script setup>
import { ref } from 'vue'
import Checkbox from "../../../Checkbox.vue";
import { useConfigStore } from '../../../stores/config.js'
import { computed } from 'vue'

const store = useConfigStore()
const config = store.config
const platform = computed(() => config.value?.platform || '')
</script>

<template>
  <div id="nvidia-nvenc-encoder" class="config-page">
    <!-- Performance preset -->
    <div class="mb-4">
      <label for="nvenc_preset" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.nvenc_preset') }}</label>
      <select id="nvenc_preset"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40"
        v-model="config.nvenc_preset">
        <option value="1">P1 {{ $t('config.nvenc_preset_1') }}</option>
        <option value="2">P2</option>
        <option value="3">P3</option>
        <option value="4">P4</option>
        <option value="5">P5</option>
        <option value="6">P6</option>
        <option value="7">P7 {{ $t('config.nvenc_preset_7') }}</option>
      </select>
      <p class="text-[11px] opacity-60 mt-1">{{ $t('config.nvenc_preset_desc') }}</p>
    </div>

    <!-- Two-pass mode -->
    <div class="mb-4">
      <label for="nvenc_twopass" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.nvenc_twopass') }}</label>
      <select id="nvenc_twopass"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40"
        v-model="config.nvenc_twopass">
        <option value="disabled">{{ $t('config.nvenc_twopass_disabled') }}</option>
        <option value="quarter_res">{{ $t('config.nvenc_twopass_quarter_res') }}</option>
        <option value="full_res">{{ $t('config.nvenc_twopass_full_res') }}</option>
      </select>
      <p class="text-[11px] opacity-60 mt-1">{{ $t('config.nvenc_twopass_desc') }}</p>
    </div>

    <!-- Spatial AQ -->
    <Checkbox class="mb-3" id="nvenc_spatial_aq" locale-prefix="config" v-model="config.nvenc_spatial_aq"
      default="false"></Checkbox>

    <!-- Single-frame VBV/HRD percentage increase -->
    <div class="mb-4">
      <label for="nvenc_vbv_increase" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.nvenc_vbv_increase') }}</label>
      <input type="number" min="0" max="400"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15" id="nvenc_vbv_increase"
        placeholder="0" v-model="config.nvenc_vbv_increase" />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.nvenc_vbv_increase_desc') }}<br>
        <br>
        <a href="https://en.wikipedia.org/wiki/Video_buffering_verifier">VBV/HRD</a>
      </p>
    </div>

    <!-- Miscellaneous options -->
    <div class="mb-4 rounded-md overflow-hidden border border-black/5 dark:border-white/10">
      <div class="bg-solar-surface/40 dark:bg-lunar-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">{{ $t('config.misc') }}</h3>
      </div>
      <div class="p-4">
        <!-- NVENC Realtime HAGS priority -->
        <Checkbox v-if="platform === 'windows'" class="mb-3" id="nvenc_realtime_hags" locale-prefix="config"
          v-model="config.nvenc_realtime_hags" default="true">
          <br>
          <br>
          <a href="https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/">HAGS</a>
        </Checkbox>

        <!-- Prefer lower encoding latency over power savings -->
        <Checkbox v-if="platform === 'windows'" class="mb-3" id="nvenc_latency_over_power" locale-prefix="config"
          v-model="config.nvenc_latency_over_power" default="true"></Checkbox>

        <!-- Present OpenGL/Vulkan on top of DXGI -->
        <Checkbox v-if="platform === 'windows'" class="mb-3" id="nvenc_opengl_vulkan_on_dxgi" locale-prefix="config"
          v-model="config.nvenc_opengl_vulkan_on_dxgi" default="true"></Checkbox>

        <!-- NVENC H264 CAVLC -->
        <Checkbox class="mb-3" id="nvenc_h264_cavlc" locale-prefix="config" v-model="config.nvenc_h264_cavlc"
          default="false"></Checkbox>
      </div>
    </div>
  </div>
</template>

<style scoped></style>
