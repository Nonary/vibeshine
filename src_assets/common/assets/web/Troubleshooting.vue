<template>
  <div class="container">
    <h1 class="my-4">
      {{ $t('troubleshooting.troubleshooting') }}
    </h1>
    <!-- Force Close App -->
    <div class="card p-2 my-4">
      <div class="card-body">
        <h2 id="close_apps">
          {{ $t('troubleshooting.force_close') }}
        </h2>
        <br />
        <p>{{ $t('troubleshooting.force_close_desc') }}</p>
        <div v-if="closeAppStatus === true" class="alert alert-success">
          {{ $t('troubleshooting.force_close_success') }}
        </div>
        <div v-if="closeAppStatus === false" class="alert alert-danger">
          {{ $t('troubleshooting.force_close_error') }}
        </div>
        <div>
          <button class="btn btn-warning" :disabled="closeAppPressed" @click="closeApp">
            {{ $t('troubleshooting.force_close') }}
          </button>
        </div>
      </div>
    </div>
    <!-- Restart Sunshine -->
    <div class="card p-2 my-4">
      <div class="card-body">
        <h2 id="restart">
          {{ $t('troubleshooting.restart_sunshine') }}
        </h2>
        <br />
        <p>{{ $t('troubleshooting.restart_sunshine_desc') }}</p>
        <div v-if="restartPressed === true" class="alert alert-success">
          {{ $t('troubleshooting.restart_sunshine_success') }}
        </div>
        <div>
          <button class="btn btn-warning" :disabled="restartPressed" @click="restart">
            {{ $t('troubleshooting.restart_sunshine') }}
          </button>
        </div>
      </div>
    </div>
    <!-- Reset persistent display device settings -->
    <div v-if="platform === 'windows'" class="card p-2 my-4">
      <div class="card-body">
        <h2 id="dd_reset">
          {{ $t('troubleshooting.dd_reset') }}
        </h2>
        <br />
        <p style="white-space: pre-line">
          {{ $t('troubleshooting.dd_reset_desc') }}
        </p>
        <div v-if="ddResetStatus === true" class="alert alert-success">
          {{ $t('troubleshooting.dd_reset_success') }}
        </div>
        <div v-if="ddResetStatus === false" class="alert alert-danger">
          {{ $t('troubleshooting.dd_reset_error') }}
        </div>
        <div>
          <button class="btn btn-warning" :disabled="ddResetPressed" @click="ddResetPersistence">
            {{ $t('troubleshooting.dd_reset') }}
          </button>
        </div>
      </div>
    </div>
    <!-- Unpair Clients -->
    <div class="card my-4">
      <div class="card-body">
        <div class="p-2">
          <div class="d-flex justify-content-end align-items-center">
            <h2 id="unpair" class="text-center me-auto">
              {{ $t('troubleshooting.unpair_title') }}
            </h2>
            <button class="btn btn-danger" :disabled="unpairAllPressed" @click="unpairAll">
              {{ $t('troubleshooting.unpair_all') }}
            </button>
          </div>
          <br />
          <p class="mb-0">
            {{ $t('troubleshooting.unpair_desc') }}
          </p>
          <div
            id="apply-alert"
            class="alert alert-success d-flex align-items-center mt-3"
            :style="{ display: showApplyMessage ? 'flex !important' : 'none !important' }"
          >
            <div class="me-2">
              <b>{{ $t('_common.success') }}</b> {{ $t('troubleshooting.unpair_single_success') }}
            </div>
            <button class="btn btn-success ms-auto apply" @click="clickedApplyBanner">
              {{ $t('_common.dismiss') }}
            </button>
          </div>
          <div v-if="unpairAllStatus === true" class="alert alert-success mt-3">
            {{ $t('troubleshooting.unpair_all_success') }}
          </div>
          <div v-if="unpairAllStatus === false" class="alert alert-danger mt-3">
            {{ $t('troubleshooting.unpair_all_error') }}
          </div>
        </div>
      </div>
      <ul
        v-if="clients && clients.length > 0"
        id="client-list"
        class="list-group list-group-flush list-group-item-light"
      >
        <div v-for="client in clients" :key="client.uuid" class="list-group-item d-flex">
          <div class="p-2 flex-grow-1">
            {{ client.name !== '' ? client.name : $t('troubleshooting.unpair_single_unknown') }}
          </div>
          <div class="me-2 ms-auto btn btn-danger" @click="unpairSingle(client.uuid)">
            <i class="fas fa-trash" />
          </div>
        </div>
      </ul>
      <ul v-else class="list-group list-group-flush list-group-item-light">
        <div class="list-group-item p-3 text-center">
          <em>{{ $t('troubleshooting.unpair_single_no_devices') }}</em>
        </div>
      </ul>
    </div>
    <!-- Logs -->
    <div class="card p-2 my-4">
      <div class="card-body">
        <h2 id="logs">
          {{ $t('troubleshooting.logs') }}
        </h2>
        <br />
        <div class="d-flex justify-content-between align-items-baseline py-2">
          <p>{{ $t('troubleshooting.logs_desc') }}</p>
          <input
            v-model="logFilter"
            type="text"
            class="form-control"
            :placeholder="$t('troubleshooting.logs_find')"
            style="width: 300px"
          />
        </div>
        <div>
          <div class="troubleshooting-logs">
            <button class="copy-icon">
              <i class="fas fa-copy" @click="copyLogs" /></button
            >{{ actualLogs }}
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onBeforeUnmount } from 'vue';
import { useConfigStore } from '@/stores/config.js';
import { http } from '@/http.js';

const store = useConfigStore();
const platform = computed(() => store.config.value?.platform || '');

const clients = ref([]);
const closeAppPressed = ref(false);
const closeAppStatus = ref(null);
const ddResetPressed = ref(false);
const ddResetStatus = ref(null);
const logs = ref('Loading...');
const logFilter = ref(null);
let logInterval = null;
const restartPressed = ref(false);
const showApplyMessage = ref(false);
const unpairAllPressed = ref(false);
const unpairAllStatus = ref(null);

const actualLogs = computed(() => {
  if (!logFilter.value) return logs.value;
  let lines = logs.value.split('\n');
  lines = lines.filter((x) => x.indexOf(logFilter.value) !== -1);
  return lines.join('\n');
});

async function refreshLogs() {
  try {
    const r = await http.get('./api/logs', {
      responseType: 'text',
      transformResponse: [(v) => v],
      validateStatus: () => true,
    });
    if (typeof r.data === 'string') logs.value = r.data;
  } catch {
    /* ignore errors */
  }
}

async function closeApp() {
  closeAppPressed.value = true;
  try {
    const r = await http.post('./api/apps/close', {}, { validateStatus: () => true });
    closeAppStatus.value = r.data?.status === true;
  } catch {
    closeAppStatus.value = false;
  } finally {
    closeAppPressed.value = false;
    setTimeout(() => {
      closeAppStatus.value = null;
    }, 5000);
  }
}

async function unpairAll() {
  unpairAllPressed.value = true;
  try {
    const r = await http.post('./api/clients/unpair-all', {}, { validateStatus: () => true });
    unpairAllStatus.value = r.data?.status === true;
  } catch {
    unpairAllStatus.value = false;
  } finally {
    unpairAllPressed.value = false;
    setTimeout(() => {
      unpairAllStatus.value = null;
    }, 5000);
    refreshClients();
  }
}

async function unpairSingle(uuid) {
  try {
    await http.post('./api/clients/unpair', { uuid }, { validateStatus: () => true });
  } catch {}
  showApplyMessage.value = true;
  refreshClients();
}

async function refreshClients() {
  try {
    const r = await http.get('./api/clients/list', { validateStatus: () => true });
    const response = r.data || {};
    if (response.status === true && response.named_certs && response.named_certs.length) {
      clients.value = response.named_certs.sort((a, b) =>
        a.name.toLowerCase() > b.name.toLowerCase() || a.name === '' ? 1 : -1,
      );
    } else {
      clients.value = [];
    }
  } catch {
    clients.value = [];
  }
}

function clickedApplyBanner() {
  showApplyMessage.value = false;
}

function copyLogs() {
  navigator.clipboard.writeText(actualLogs.value);
}

function restart() {
  restartPressed.value = true;
  setTimeout(() => {
    restartPressed.value = false;
  }, 5000);
  http.post('./api/restart', {}, { validateStatus: () => true });
}

async function ddResetPersistence() {
  ddResetPressed.value = true;
  try {
    const r = await http.post(
      '/api/reset-display-device-persistence',
      {},
      { validateStatus: () => true },
    );
    ddResetStatus.value = r.data?.status === true;
  } catch {
    ddResetStatus.value = false;
  } finally {
    ddResetPressed.value = false;
    setTimeout(() => {
      ddResetStatus.value = null;
    }, 5000);
  }
}

onMounted(() => {
  // If store has no config yet, use the store helper to fetch it so platform is available
  if (!store.config.value) {
    store.fetchConfig().catch(() => {
      /* ignore errors */
    });
  }
  logInterval = setInterval(() => refreshLogs(), 5000);
  refreshLogs();
  refreshClients();
});

onBeforeUnmount(() => {
  if (logInterval) clearInterval(logInterval);
});
</script>

<style scoped>
.troubleshooting-logs {
  white-space: pre;
  font-family: monospace;
  overflow: auto;
  max-height: 500px;
  min-height: 500px;
  font-size: 16px;
  position: relative;
}

.copy-icon {
  position: absolute;
  top: 8px;
  right: 8px;
  padding: 8px;
  cursor: pointer;
  color: rgba(0, 0, 0, 1);
  appearance: none;
  border: none;
  background: none;
}

.copy-icon:hover {
  color: rgba(0, 0, 0, 0.75);
}

.copy-icon:active {
  color: rgba(0, 0, 0, 1);
}
</style>
