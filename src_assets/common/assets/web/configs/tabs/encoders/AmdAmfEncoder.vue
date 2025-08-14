<script setup>
import { ref } from 'vue'
import Checkbox from "../../../Checkbox.vue";
import { useConfigStore } from '../../../stores/config.js'

const store = useConfigStore()
const config = store.config
</script>

<template>
  <div id="amd-amf-encoder" class="config-page">
    <!-- AMF Usage -->
    <div class="mb-4">
      <label for="amf_quality" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.amf.quality') }}</label>
      <select id="amf_quality" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40" v-model="config.profile">
        <option value="transcoding">{{ $t('config.amd_usage_transcoding') }}</option>
        <option value="webcam">{{ $t('config.amd_usage_webcam') }}</option>
        <option value="lowlatency_high_quality">{{ $t('config.amd_usage_lowlatency_high_quality') }}</option>
        <option value="lowlatency">{{ $t('config.amd_usage_lowlatency') }}</option>
        <option value="ultralowlatency">{{ $t('config.amd_usage_ultralowlatency') }}</option>
      </select>
      <div class="form-text">{{ $t('config.amd_usage_desc') }}</div>
    </div>

    <!-- AMD Rate Control group options -->
    <div class="mb-4 rounded-md overflow-hidden border border-black/5 dark:border-white/10">
      <div class="bg-solar-surface/40 dark:bg-lunar-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">{{ $t('config.amd_rc_group') }}</h3>
      </div>
      <div class="p-4">
        <!-- AMF Rate Control -->
        <div class="mb-4">
          <label for="amf_rate_control" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.amf.rate_control') }}</label>
          <select id="amf_rate_control" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40" v-model="config.rate_control">
            <option value="cbr">{{ $t('config.amd_rc_cbr') }}</option>
            <option value="cqp">{{ $t('config.amd_rc_cqp') }}</option>
            <option value="vbr_latency">{{ $t('config.amd_rc_vbr_latency') }}</option>
            <option value="vbr_peak">{{ $t('config.amd_rc_vbr_peak') }}</option>
          </select>
          <p class="text-[11px] opacity-60 mt-1">{{ $t('config.amd_rc_desc') }}</p>
        </div>

        <!-- AMF HRD Enforcement -->
        <Checkbox class="mb-3"
                  id="amd_enforce_hrd"
                  locale-prefix="config"
                  v-model="config.amd_enforce_hrd"
                  default="false"
        ></Checkbox>
      </div>
    </div>

    <!-- AMF Quality group options -->
    <div class="mb-4 rounded-md overflow-hidden border border-black/5 dark:border-white/10">
      <div class="bg-solar-surface/40 dark:bg-lunar-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">{{ $t('config.amd_quality_group') }}</h3>
      </div>
      <div class="p-4">
        <!-- AMF Quality -->
        <div class="mb-6">
          <label for="amd_quality" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.amd_quality') }}</label>
          <select id="amd_quality" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40" v-model="config.amd_quality">
            <option value="speed">{{ $t('config.amd_quality_speed') }}</option>
            <option value="balanced">{{ $t('config.amd_quality_balanced') }}</option>
            <option value="quality">{{ $t('config.amd_quality_quality') }}</option>
          </select>
          <p class="text-[11px] opacity-60 mt-1">{{ $t('config.amd_quality_desc') }}</p>
        </div>

        <!-- AMD Preanalysis -->
        <Checkbox class="mb-3"
                  id="amd_preanalysis"
                  locale-prefix="config"
                  v-model="config.amd_preanalysis"
                  default="false"
        ></Checkbox>

        <!-- AMD VBAQ -->
        <Checkbox class="mb-3"
                  id="amd_vbaq"
                  locale-prefix="config"
                  v-model="config.amd_vbaq"
                  default="true"
        ></Checkbox>

        <!-- AMF Coder (H264) -->
        <div class="mb-6">
          <label for="amd_coder" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.amd_coder') }}</label>
          <select id="amd_coder" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40" v-model="config.amd_coder">
            <option value="auto">{{ $t('config.ffmpeg_auto') }}</option>
            <option value="cabac">{{ $t('config.coder_cabac') }}</option>
            <option value="cavlc">{{ $t('config.coder_cavlc') }}</option>
          </select>
          <p class="text-[11px] opacity-60 mt-1">{{ $t('config.amd_coder_desc') }}</p>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>

</style>
