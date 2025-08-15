<template>
  <div class="container mx-auto max-w-4xl">
    <h1 class="text-2xl font-semibold my-4 flex items-center gap-3">
      <i class="fas fa-users-cog" /> {{ $t('clients.title') }}
    </h1>

    <!-- Pair New Client -->
    <div
      class="card p-4 mb-6 shadow-sm border border-solar-dark/10 dark:border-lunar-light/10 rounded-md bg-white dark:bg-lunar-surface">
      <h2 class="text-lg font-medium mb-2 flex items-center gap-2">
        <i class="fas fa-link" /> {{ $t('clients.pair_title') }}
      </h2>
      <p class="text-sm opacity-75 mb-3">{{ $t('clients.pair_desc') }}</p>
      <form @submit.prevent="registerDevice" class="flex flex-col gap-3 md:flex-row md:items-end">
        <div class="flex flex-col flex-1">
          <label class="text-xs font-semibold uppercase tracking-wide mb-1" for="pin-input">{{ $t('navbar.pin')
            }}</label>
          <input id="pin-input" v-model="pin" type="text" inputmode="numeric" pattern="\\d*" maxlength="4"
            class="form-control px-3 py-2 rounded border focus:outline-none focus:ring w-full"
            :placeholder="$t('navbar.pin')" required>
        </div>
        <div class="flex flex-col flex-1">
          <label class="text-xs font-semibold uppercase tracking-wide mb-1" for="name-input">{{ $t('pin.device_name')
            }}</label>
          <input id="name-input" v-model="deviceName" type="text"
            class="form-control px-3 py-2 rounded border focus:outline-none focus:ring w-full"
            :placeholder="$t('pin.device_name')" required>
        </div>
        <div class="flex flex-col">
          <button :disabled="pairing" class="btn btn-primary px-4 py-2 mt-2 md:mt-0">
            <span v-if="!pairing">{{ $t('pin.send') }}</span>
            <span v-else>{{ $t('clients.pairing') }}</span>
          </button>
        </div>
      </form>
      <div class="mt-3">
        <div v-if="pairStatus === true" class="alert alert-success">{{ $t('pin.pair_success') }}</div>
        <div v-if="pairStatus === false" class="alert alert-danger">{{ $t('pin.pair_failure') }}</div>
      </div>
      <div class="alert alert-warning mt-4 text-sm flex items-start gap-2">
        <b>{{ $t('_common.warning') }}</b>
        <span>{{ $t('pin.warning_msg') }}</span>
      </div>
    </div>

    <!-- Existing Clients -->
    <div
      class="card p-4 shadow-sm border border-solar-dark/10 dark:border-lunar-light/10 rounded-md bg-white dark:bg-lunar-surface mb-6">
      <div class="flex items-center gap-3 mb-3">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-users" /> {{ $t('clients.existing_title') }}
        </h2>
        <button class="btn btn-danger ms-auto" :disabled="unpairAllPressed || clients.length === 0" @click="unpairAll">
          <i class="fas fa-user-slash" /> {{ $t('troubleshooting.unpair_all') }}
        </button>
      </div>
      <p class="text-sm opacity-75 mb-3">{{ $t('troubleshooting.unpair_desc') }}</p>
      <div v-if="unpairAllStatus === true" class="alert alert-success mb-3">{{ $t('troubleshooting.unpair_all_success')
        }}</div>
      <div v-if="unpairAllStatus === false" class="alert alert-danger mb-3">{{ $t('troubleshooting.unpair_all_error') }}
      </div>
      <ul v-if="clients && clients.length > 0" class="list-group list-group-flush list-group-item-light divide-y">
        <li v-for="client in clients" :key="client.uuid"
          class="flex items-center py-2 px-2 hover:bg-solar-primary/5 dark:hover:bg-lunar-primary/10">
          <div class="flex-1">
            {{ client.name !== '' ? client.name : $t('troubleshooting.unpair_single_unknown') }}
          </div>
          <button class="btn btn-danger btn-sm" :disabled="removing[client.uuid] === true"
            @click="unpairSingle(client.uuid)">
            <i class="fas fa-trash" />
          </button>
        </li>
      </ul>
      <div v-else class="p-4 text-center italic opacity-75">
        {{ $t('troubleshooting.unpair_single_no_devices') }}
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { http } from '@/http.js'

const clients = ref([])
const pin = ref('')
const deviceName = ref('')
const pairing = ref(false)
const pairStatus = ref(null) // true/false/null
const unpairAllPressed = ref(false)
const unpairAllStatus = ref(null)
const removing = ref({})

async function refreshClients() {
  try {
    const r = await http.get('./api/clients/list', { validateStatus: () => true })
    const response = r.data || {}
    if (response.status === true && response.named_certs && response.named_certs.length) {
      clients.value = response.named_certs.sort((a, b) => (a.name.toLowerCase() > b.name.toLowerCase() || a.name === '' ? 1 : -1))
    } else { clients.value = [] }
  } catch { clients.value = [] }
}

async function registerDevice() {
  if (pairing.value) return
  pairStatus.value = null
  pairing.value = true
  try {
    const body = { pin: pin.value.trim(), name: deviceName.value.trim() }
    const r = await http.post('./api/pin', body, { validateStatus: () => true })
    pairStatus.value = r.data?.status === true
    if (pairStatus.value) {
      pin.value = ''
      deviceName.value = ''
      refreshClients()
    }
  } catch { pairStatus.value = false }
  finally { pairing.value = false; setTimeout(() => { pairStatus.value = null }, 5000) }
}

async function unpairSingle(uuid) {
  if (removing.value[uuid]) return
  removing.value = { ...removing.value, [uuid]: true }
  try { await http.post('./api/clients/unpair', { uuid }, { validateStatus: () => true }) } catch { }
  finally { delete removing.value[uuid]; removing.value = { ...removing.value }; refreshClients() }
}

async function unpairAll() {
  unpairAllPressed.value = true
  try {
    const r = await http.post('./api/clients/unpair-all', {}, { validateStatus: () => true })
    unpairAllStatus.value = r.data?.status === true
  } catch { unpairAllStatus.value = false }
  finally { unpairAllPressed.value = false; setTimeout(() => { unpairAllStatus.value = null }, 5000); refreshClients() }
}

onMounted(() => refreshClients())
</script>

<style scoped>
/* Basic approximations replacing Tailwind utility aggregates */
.card {
  background: var(--color-surface, #ffffff);
}

.btn {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
  border: none;
  border-radius: 0.375rem;
  font-weight: 500;
  cursor: pointer;
}

.btn-primary {
  background: #ffc400;
  color: #fff;
  padding: 0.5rem 1rem;
}

.btn-primary:disabled {
  opacity: .5;
  cursor: not-allowed;
}

.btn-primary:not(:disabled):hover {
  filter: brightness(0.95);
}

.btn-danger {
  background: #dc2626;
  color: #fff;
  padding: 0.5rem 0.75rem;
}

.btn-danger:disabled {
  opacity: .5;
  cursor: not-allowed;
}

.btn-danger:not(:disabled):hover {
  background: #b91c1c;
}

.btn-sm {
  font-size: .875rem;
  padding: 0.25rem 0.5rem;
}

.alert {
  padding: 0.5rem 0.75rem;
  border-radius: 0.375rem;
  font-size: .875rem;
}

.alert-success {
  background: #d1fae5;
  color: #065f46;
}

.alert-danger {
  background: #fee2e2;
  color: #991b1b;
}

.alert-warning {
  background: #fef3c7;
  color: #92400e;
}
</style>
