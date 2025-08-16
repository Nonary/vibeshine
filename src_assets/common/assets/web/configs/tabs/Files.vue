<script setup>
import { ref } from 'vue';
import { useConfigStore } from '@/stores/config.js';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="files" class="config-page">
    <!-- Apps File -->
    <div class="mb-6">
      <label for="file_apps" class="form-label">{{ $t('config.file_apps') }}</label>
      <input
        id="file_apps"
        v-model="config.file_apps"
        type="text"
        class="form-control"
        placeholder="apps.json"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.file_apps_desc') }}
      </div>
    </div>

    <!-- Credentials File -->
    <div class="mb-6">
      <label for="credentials_file" class="form-label">{{ $t('config.credentials_file') }}</label>
      <input
        id="credentials_file"
        v-model="config.credentials_file"
        type="text"
        class="form-control"
        placeholder="sunshine_state.json"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.credentials_file_desc') }}
      </div>
    </div>

    <!-- Log Path -->
    <div class="mb-6">
      <label for="log_path" class="form-label">{{ $t('config.log_path') }}</label>
      <input
        id="log_path"
        v-model="config.log_path"
        type="text"
        class="form-control"
        placeholder="sunshine.log"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.log_path_desc') }}
      </div>
    </div>

    <!-- Private Key -->
    <div class="mb-6">
      <label for="pkey" class="form-label">{{ $t('config.pkey') }}</label>
      <input
        id="pkey"
        v-model="config.pkey"
        type="text"
        class="form-control"
        placeholder="/dir/pkey.pem"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.pkey_desc') }}
      </div>
    </div>

    <!-- Certificate -->
    <div class="mb-6">
      <label for="cert" class="form-label">{{ $t('config.cert') }}</label>
      <input
        id="cert"
        v-model="config.cert"
        type="text"
        class="form-control"
        placeholder="/dir/cert.pem"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.cert_desc') }}
      </div>
    </div>

    <!-- State File -->
    <div class="mb-6">
      <label for="file_state" class="form-label">{{ $t('config.file_state') }}</label>
      <input
        id="file_state"
        v-model="config.file_state"
        type="text"
        class="form-control"
        placeholder="sunshine_state.json"
      />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.file_state_desc') }}
      </div>
    </div>
  </div>
</template>

<style scoped></style>
