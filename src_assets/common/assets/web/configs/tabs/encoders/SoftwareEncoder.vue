<script setup>
import { ref } from 'vue'
import { useConfigStore } from '@/stores/config.js'

const store = useConfigStore()
const config = store.config
</script>

<template>
  <div id="software-encoder" class="config-page">
    <div class="mb-4">
      <label for="sw_preset" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.sw_preset') }}</label>
      <select id="sw_preset" v-model="config.sw_preset"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40">
        <option value="ultrafast">
          {{ $t('config.sw_preset_ultrafast') }}
        </option>
        <option value="superfast">
          {{ $t('config.sw_preset_superfast') }}
        </option>
        <option value="veryfast">
          {{ $t('config.sw_preset_veryfast') }}
        </option>
        <option value="faster">
          {{ $t('config.sw_preset_faster') }}
        </option>
        <option value="fast">
          {{ $t('config.sw_preset_fast') }}
        </option>
        <option value="medium">
          {{ $t('config.sw_preset_medium') }}
        </option>
        <option value="slow">
          {{ $t('config.sw_preset_slow') }}
        </option>
        <option value="slower">
          {{ $t('config.sw_preset_slower') }}
        </option>
        <option value="veryslow">
          {{ $t('config.sw_preset_veryslow') }}
        </option>
      </select>
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.sw_preset_desc') }}
      </p>
    </div>

    <div class="mb-4">
      <label for="sw_tune" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.sw_tune') }}</label>
      <select id="sw_tune" v-model="config.sw_tune"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40">
        <option value="film">
          {{ $t('config.sw_tune_film') }}
        </option>
        <option value="animation">
          {{ $t('config.sw_tune_animation') }}
        </option>
        <option value="grain">
          {{ $t('config.sw_tune_grain') }}
        </option>
        <option value="stillimage">
          {{ $t('config.sw_tune_stillimage') }}
        </option>
        <option value="fastdecode">
          {{ $t('config.sw_tune_fastdecode') }}
        </option>
        <option value="zerolatency">
          {{ $t('config.sw_tune_zerolatency') }}
        </option>
      </select>
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.sw_tune_desc') }}
      </p>
    </div>
  </div>
</template>

<style scoped></style>
