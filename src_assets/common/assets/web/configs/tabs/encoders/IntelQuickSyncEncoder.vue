<script setup>
import { ref } from 'vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config.js';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="intel-quicksync-encoder" class="config-page">
    <!-- QuickSync Preset -->
    <div class="mb-4">
      <label for="qsv_preset" class="form-label">{{ $t('config.qsv_preset') }}</label>
      <select id="qsv_preset" v-model="config.qsv_preset" class="form-control">
        <option value="veryfast">
          {{ $t('config.qsv_preset_veryfast') }}
        </option>
        <option value="faster">
          {{ $t('config.qsv_preset_faster') }}
        </option>
        <option value="fast">
          {{ $t('config.qsv_preset_fast') }}
        </option>
        <option value="medium">
          {{ $t('config.qsv_preset_medium') }}
        </option>
        <option value="slow">
          {{ $t('config.qsv_preset_slow') }}
        </option>
        <option value="slower">
          {{ $t('config.qsv_preset_slower') }}
        </option>
        <option value="slowest">
          {{ $t('config.qsv_preset_slowest') }}
        </option>
      </select>
    </div>

    <!-- QuickSync Coder (H264) -->
    <div class="mb-4">
      <label for="qsv_coder" class="form-label">{{ $t('config.qsv_coder') }}</label>
      <select id="qsv_coder" v-model="config.qsv_coder" class="form-control">
        <option value="auto">
          {{ $t('config.ffmpeg_auto') }}
        </option>
        <option value="cabac">
          {{ $t('config.coder_cabac') }}
        </option>
        <option value="cavlc">
          {{ $t('config.coder_cavlc') }}
        </option>
      </select>
    </div>

    <!-- Allow Slow HEVC Encoding -->
    <Checkbox id="qsv_slow_hevc" v-model="config.qsv_slow_hevc" class="mb-3" locale-prefix="config" default="false" />
  </div>
</template>

<style scoped></style>
