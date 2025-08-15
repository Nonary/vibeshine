<script setup>
import { ref } from 'vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config.js';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="videotoolbox-encoder" class="config-page">
    <!-- Presets -->
    <div class="mb-4">
      <label for="vt_coder" class="form-label">{{ $t('config.vt_coder') }}</label>
      <select id="vt_coder" v-model="config.vt_coder" class="form-control">
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
    <div class="mb-4">
      <label for="vt_software" class="form-label">{{ $t('config.vt_software') }}</label>
      <select id="vt_software" v-model="config.vt_software" class="form-control">
        <option value="auto">
          {{ $t('_common.auto') }}
        </option>
        <option value="disabled">
          {{ $t('_common.disabled') }}
        </option>
        <option value="allowed">
          {{ $t('config.vt_software_allowed') }}
        </option>
        <option value="forced">
          {{ $t('config.vt_software_forced') }}
        </option>
      </select>
    </div>
    <Checkbox id="vt_realtime" v-model="config.vt_realtime" class="mb-4" desc="" locale-prefix="config"
      default="true" />
  </div>
</template>

<style scoped></style>
