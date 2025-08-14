<script setup>
import { ref } from 'vue'
import Checkbox from "../../../Checkbox.vue";
import { useConfigStore } from '../../../stores/config.js'

const store = useConfigStore()
const config = store.config
</script>

<template>
  <div id="videotoolbox-encoder" class="config-page">
    <!-- Presets -->
    <div class="mb-4">
      <label for="vt_coder" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.vt_coder') }}</label>
      <select id="vt_coder" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40" v-model="config.vt_coder">
        <option value="auto">{{ $t('config.ffmpeg_auto') }}</option>
        <option value="cabac">{{ $t('config.coder_cabac') }}</option>
        <option value="cavlc">{{ $t('config.coder_cavlc') }}</option>
      </select>
    </div>
    <div class="mb-4">
      <label for="vt_software" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.vt_software') }}</label>
      <select id="vt_software" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40" v-model="config.vt_software">
        <option value="auto">{{ $t('_common.auto') }}</option>
        <option value="disabled">{{ $t('_common.disabled') }}</option>
        <option value="allowed">{{ $t('config.vt_software_allowed') }}</option>
        <option value="forced">{{ $t('config.vt_software_forced') }}</option>
      </select>
    </div>
  <Checkbox class="mb-4"
              id="vt_realtime"
              desc=""
              locale-prefix="config"
              v-model="config.vt_realtime"
              default="true"
    ></Checkbox>
  </div>
</template>

<style scoped>

</style>
