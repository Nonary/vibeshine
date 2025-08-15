<script setup>
import { ref } from 'vue'
import { useConfigStore } from '@/stores/config.js'

const store = useConfigStore()
const config = store.config
</script>

<template>
  <div id="files" class="config-page">
    <!-- Apps File -->
    <div class="mb-6">
      <label for="file_apps" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.file_apps') }}</label>
      <input id="file_apps" v-model="config.file_apps" type="text"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70"
        placeholder="apps.json">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.file_apps_desc') }}
      </div>
    </div>

    <!-- Credentials File -->
    <div class="mb-6">
      <label for="credentials_file" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.credentials_file') }}</label>
      <input id="credentials_file" v-model="config.credentials_file" type="text"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70"
        placeholder="sunshine_state.json">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.credentials_file_desc') }}
      </div>
    </div>

    <!-- Log Path -->
    <div class="mb-6">
      <label for="log_path" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.log_path') }}</label>
      <input id="log_path" v-model="config.log_path" type="text"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70"
        placeholder="sunshine.log">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.log_path_desc') }}
      </div>
    </div>

    <!-- Private Key -->
    <div class="mb-6">
      <label for="pkey" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.pkey') }}</label>
      <input id="pkey" v-model="config.pkey" type="text"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70"
        placeholder="/dir/pkey.pem">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.pkey_desc') }}
      </div>
    </div>

    <!-- Certificate -->
    <div class="mb-6">
      <label for="cert" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.cert') }}</label>
      <input id="cert" v-model="config.cert" type="text"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70"
        placeholder="/dir/cert.pem">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.cert_desc') }}
      </div>
    </div>

    <!-- State File -->
    <div class="mb-6">
      <label for="file_state" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.file_state') }}</label>
      <input id="file_state" v-model="config.file_state" type="text"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70"
        placeholder="sunshine_state.json">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.file_state_desc') }}
      </div>
    </div>
  </div>
</template>

<style scoped></style>
