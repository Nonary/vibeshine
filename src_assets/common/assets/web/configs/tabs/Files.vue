<script setup>
import { ref } from 'vue'
import { useConfigStore } from '../../stores/config.js'

const store = useConfigStore()
const config = store.config
</script>

<template>
  <div id="files" class="config-page">
    <!-- Apps File -->
    <div class="mb-6">
      <label for="file_apps" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.file_apps') }}</label>
      <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70" id="file_apps" placeholder="apps.json" v-model="config.file_apps" />
      <div class="text-[11px] opacity-60 mt-1">{{ $t('config.file_apps_desc') }}</div>
    </div>

    <!-- Credentials File -->
    <div class="mb-6">
      <label for="credentials_file" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.credentials_file') }}</label>
      <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70" id="credentials_file" placeholder="sunshine_state.json" v-model="config.credentials_file" />
      <div class="text-[11px] opacity-60 mt-1">{{ $t('config.credentials_file_desc') }}</div>
    </div>

    <!-- Log Path -->
    <div class="mb-6">
      <label for="log_path" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.log_path') }}</label>
      <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70" id="log_path" placeholder="sunshine.log" v-model="config.log_path" />
      <div class="text-[11px] opacity-60 mt-1">{{ $t('config.log_path_desc') }}</div>
    </div>

    <!-- Private Key -->
    <div class="mb-6">
      <label for="pkey" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.pkey') }}</label>
      <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70" id="pkey" placeholder="/dir/pkey.pem" v-model="config.pkey" />
      <div class="text-[11px] opacity-60 mt-1">{{ $t('config.pkey_desc') }}</div>
    </div>

    <!-- Certificate -->
    <div class="mb-6">
      <label for="cert" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.cert') }}</label>
      <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70" id="cert" placeholder="/dir/cert.pem" v-model="config.cert" />
      <div class="text-[11px] opacity-60 mt-1">{{ $t('config.cert_desc') }}</div>
    </div>

    <!-- State File -->
    <div class="mb-6">
      <label for="file_state" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.file_state') }}</label>
      <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70" id="file_state" placeholder="sunshine_state.json"
             v-model="config.file_state" />
      <div class="text-[11px] opacity-60 mt-1">{{ $t('config.file_state_desc') }}</div>
    </div>

  </div>
</template>

<style scoped>

</style>
