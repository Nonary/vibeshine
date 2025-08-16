<script setup>
import { ref } from 'vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config.js';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="amd-amf-encoder" class="config-page">
    <!-- AMF Usage -->
    <div class="mb-4">
      <label for="amf_quality" class="form-label">{{ $t('config.amf.quality') }}</label>
      <select id="amf_quality" v-model="config.profile" class="form-control">
        <option value="transcoding">
          {{ $t('config.amd_usage_transcoding') }}
        </option>
        <option value="webcam">
          {{ $t('config.amd_usage_webcam') }}
        </option>
        <option value="lowlatency_high_quality">
          {{ $t('config.amd_usage_lowlatency_high_quality') }}
        </option>
        <option value="lowlatency">
          {{ $t('config.amd_usage_lowlatency') }}
        </option>
        <option value="ultralowlatency">
          {{ $t('config.amd_usage_ultralowlatency') }}
        </option>
      </select>
      <div class="form-text">
        {{ $t('config.amd_usage_desc') }}
      </div>
    </div>

    <!-- AMD Rate Control group options -->
    <div class="mb-4 rounded-md overflow-hidden border border-black/5 dark:border-white/10">
      <div class="bg-surface/40 dark:bg-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">
          {{ $t('config.amd_rc_group') }}
        </h3>
      </div>
      <div class="p-4">
        <!-- AMF Rate Control -->
        <div class="mb-4">
          <label for="amf_rate_control" class="form-label">{{
            $t('config.amf.rate_control')
          }}</label>
          <select id="amf_rate_control" v-model="config.rate_control" class="form-control">
            <option value="cbr">
              {{ $t('config.amd_rc_cbr') }}
            </option>
            <option value="cqp">
              {{ $t('config.amd_rc_cqp') }}
            </option>
            <option value="vbr_latency">
              {{ $t('config.amd_rc_vbr_latency') }}
            </option>
            <option value="vbr_peak">
              {{ $t('config.amd_rc_vbr_peak') }}
            </option>
          </select>
          <p class="text-[11px] opacity-60 mt-1">
            {{ $t('config.amd_rc_desc') }}
          </p>
        </div>

        <!-- AMF HRD Enforcement -->
        <Checkbox
          id="amd_enforce_hrd"
          v-model="config.amd_enforce_hrd"
          class="mb-3"
          locale-prefix="config"
          default="false"
        />
      </div>
    </div>

    <!-- AMF Quality group options -->
    <div class="mb-4 rounded-md overflow-hidden border border-black/5 dark:border-white/10">
      <div class="bg-surface/40 dark:bg-surface/30 px-4 py-3">
        <h3 class="text-sm font-medium">
          {{ $t('config.amd_quality_group') }}
        </h3>
      </div>
      <div class="p-4">
        <!-- AMF Quality -->
        <div class="mb-6">
          <label for="amd_quality" class="form-label">{{ $t('config.amd_quality') }}</label>
          <select id="amd_quality" v-model="config.amd_quality" class="form-control">
            <option value="speed">
              {{ $t('config.amd_quality_speed') }}
            </option>
            <option value="balanced">
              {{ $t('config.amd_quality_balanced') }}
            </option>
            <option value="quality">
              {{ $t('config.amd_quality_quality') }}
            </option>
          </select>
          <p class="text-[11px] opacity-60 mt-1">
            {{ $t('config.amd_quality_desc') }}
          </p>
        </div>

        <!-- AMD Preanalysis -->
        <Checkbox
          id="amd_preanalysis"
          v-model="config.amd_preanalysis"
          class="mb-3"
          locale-prefix="config"
          default="false"
        />

        <!-- AMD VBAQ -->
        <Checkbox
          id="amd_vbaq"
          v-model="config.amd_vbaq"
          class="mb-3"
          locale-prefix="config"
          default="true"
        />

        <!-- AMF Coder (H264) -->
        <div class="mb-6">
          <label for="amd_coder" class="form-label">{{ $t('config.amd_coder') }}</label>
          <select id="amd_coder" v-model="config.amd_coder" class="form-control">
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
          <p class="text-[11px] opacity-60 mt-1">
            {{ $t('config.amd_coder_desc') }}
          </p>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped></style>
