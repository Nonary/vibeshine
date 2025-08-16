<script setup>
import { ref } from 'vue';
import PlatformLayout from '@/PlatformLayout.vue';
import { useConfigStore } from '@/stores/config';
import { computed } from 'vue';

const store = useConfigStore();
const config = store.config;
const platform = computed(() => config.value?.platform || '');
</script>

<template>
  <div class="config-page">
    <!-- FEC Percentage -->
    <div class="mb-6">
      <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
      <input
        id="fec_percentage"
        v-model="config.fec_percentage"
        type="text"
        class="form-control"
        placeholder="20"
      />
      <div class="form-text">{{ $t('config.fec_percentage_desc') }}</div>
    </div>

    <!-- Quantization Parameter -->
    <div class="mb-6">
      <label for="qp" class="form-label">{{ $t('config.qp') }}</label>
      <input id="qp" v-model="config.qp" type="number" class="form-control" placeholder="28" />
      <div class="form-text">{{ $t('config.qp_desc') }}</div>
    </div>

    <!-- Min Threads -->
    <div class="mb-6">
      <label for="min_threads" class="form-label">{{ $t('config.min_threads') }}</label>
      <input
        id="min_threads"
        v-model="config.min_threads"
        type="number"
        class="form-control"
        placeholder="2"
        min="1"
      />
      <div class="form-text">{{ $t('config.min_threads_desc') }}</div>
    </div>

    <!-- HEVC Support -->
    <div class="mb-6">
      <label for="hevc_mode" class="form-label">{{ $t('config.hevc_mode') }}</label>
      <select id="hevc_mode" v-model="config.hevc_mode" class="form-control">
        <option value="0">{{ $t('config.hevc_mode_0') }}</option>
        <option value="1">{{ $t('config.hevc_mode_1') }}</option>
        <option value="2">{{ $t('config.hevc_mode_2') }}</option>
        <option value="3">{{ $t('config.hevc_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.hevc_mode_desc') }}</div>
    </div>

    <!-- AV1 Support -->
    <div class="mb-6">
      <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
      <select id="av1_mode" v-model="config.av1_mode" class="form-control">
        <option value="0">{{ $t('config.av1_mode_0') }}</option>
        <option value="1">{{ $t('config.av1_mode_1') }}</option>
        <option value="2">{{ $t('config.av1_mode_2') }}</option>
        <option value="3">{{ $t('config.av1_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.av1_mode_desc') }}</div>
    </div>

    <!-- Capture -->
    <div v-if="platform !== 'macos'" class="mb-6">
      <label for="capture" class="form-label">{{ $t('config.capture') }}</label>
      <select id="capture" v-model="config.capture" class="form-control">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout>
          <template #linux>
            <option value="nvfbc">NvFBC</option>
            <option value="wlr">wlroots</option>
            <option value="kms">KMS</option>
            <option value="x11">X11</option>
          </template>
          <template #windows>
            <option value="ddx">Desktop Duplication API</option>
            <option value="wgc">Windows.Graphics.Capture Variable Framerate</option>
            <option value="wgcc">Windows.Graphics.Capture Constant Framerate</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.capture_desc') }}</div>
    </div>

    <!-- Encoder -->
    <div class="mb-6">
      <label for="encoder" class="form-label">{{ $t('config.encoder') }}</label>
      <select id="encoder" v-model="config.encoder" class="form-control">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout>
          <template #windows>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="quicksync">Intel QuickSync</option>
            <option value="amdvce">AMD AMF/VCE</option>
          </template>
          <template #linux>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="vaapi">VA-API</option>
          </template>
          <template #macos>
            <option value="videotoolbox">VideoToolbox</option>
          </template>
        </PlatformLayout>
        <option value="software">{{ $t('config.encoder_software') }}</option>
      </select>
      <div class="form-text">{{ $t('config.encoder_desc') }}</div>
    </div>
  </div>
</template>

<style scoped></style>
