<template>
  <div class="mx-auto max-w-4xl px-4">
    <h1 class="text-2xl font-semibold my-6 flex items-center gap-3 text-brand">
      <i class="fas fa-users-cog" /> {{ $t('clients.title') }}
    </h1>

    <!-- Pair New Client -->
    <UiCard class="mb-8">
      <template #title>
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-link" /> {{ $t('clients.pair_title') }}
        </h2>
      </template>
      <div class="space-y-4">
        <p class="text-sm opacity-75">{{ $t('clients.pair_desc') }}</p>
        <form
          class="grid grid-cols-1 md:grid-cols-3 gap-4 items-end"
          @submit.prevent="registerDevice"
        >
          <div class="flex flex-col">
            <label class="text-[11px] font-semibold tracking-wide uppercase mb-1" for="pin-input">{{
              $t('navbar.pin')
            }}</label>
            <input
              id="pin-input"
              v-model="pin"
              type="text"
              inputmode="numeric"
              pattern="^[0-9]{4}$"
              title="Enter 4 digits"
              maxlength="4"
              class="form-control"
              :placeholder="$t('navbar.pin')"
              required
            />
          </div>
          <div class="flex flex-col">
            <label
              class="text-[11px] font-semibold tracking-wide uppercase mb-1"
              for="name-input"
              >{{ $t('pin.device_name') }}</label
            >
            <input
              id="name-input"
              v-model="deviceName"
              type="text"
              class="form-control"
              :placeholder="$t('pin.device_name')"
              required
            />
          </div>
          <div class="flex flex-col md:items-end">
            <UiButton :disabled="pairing" class="w-full md:w-auto" variant="primary" type="submit">
              <span v-if="!pairing">{{ $t('pin.send') }}</span>
              <span v-else>{{ $t('clients.pairing') }}</span>
            </UiButton>
          </div>
        </form>
        <div class="space-y-2">
          <UiAlert v-if="pairStatus === true" variant="success">{{
            $t('pin.pair_success')
          }}</UiAlert>
          <UiAlert v-if="pairStatus === false" variant="danger">{{
            $t('pin.pair_failure')
          }}</UiAlert>
        </div>
        <UiAlert variant="warning" class="text-sm flex items-start gap-2">
          <template #default>
            <div class="flex items-start gap-2">
              <b class="font-semibold">{{ $t('_common.warning') }}</b>
              <span>{{ $t('pin.warning_msg') }}</span>
            </div>
          </template>
        </UiAlert>
      </div>
    </UiCard>

    <!-- Existing Clients -->
    <UiCard class="mb-8">
      <template #title>
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-users" /> {{ $t('clients.existing_title') }}
        </h2>
      </template>
      <template #actions>
        <UiButton
          class="ml-auto"
          variant="danger"
          :disabled="unpairAllPressed || clients.length === 0"
          @click="askConfirmUnpairAll"
        >
          <template #icon>
            <i class="fas fa-user-slash" />
          </template>
          {{ $t('troubleshooting.unpair_all') }}
        </UiButton>
      </template>
      <p class="text-sm opacity-75 mb-3">{{ $t('troubleshooting.unpair_desc') }}</p>
      <UiAlert v-if="unpairAllStatus === true" variant="success" class="mb-3">{{
        $t('troubleshooting.unpair_all_success')
      }}</UiAlert>
      <UiAlert v-if="unpairAllStatus === false" variant="danger" class="mb-3">{{
        $t('troubleshooting.unpair_all_error')
      }}</UiAlert>
      <ul v-if="clients && clients.length > 0" class="divide-y divide-dark/10 dark:divide-light/10">
        <li
          v-for="client in clients"
          :key="client.uuid"
          class="flex items-center py-2 px-2 rounded hover:bg-primary/5 transition"
        >
          <div class="flex-1 truncate">
            {{ client.name !== '' ? client.name : $t('troubleshooting.unpair_single_unknown') }}
          </div>
          <UiButton
            variant="danger"
            size="sm"
            :disabled="removing[client.uuid] === true"
            aria-label="Remove"
            @click="askConfirmUnpair(client.uuid)"
          >
            <i class="fas fa-trash" />
          </UiButton>
        </li>
      </ul>
      <div v-else class="p-4 text-center italic opacity-75">
        {{ $t('troubleshooting.unpair_single_no_devices') }}
      </div>
    </UiCard>

    <ApiTokenManager></ApiTokenManager>

    <!-- Confirm remove single client -->
    <UiConfirmModal
      v-model:open="showConfirmRemove"
      :title="
        $t('clients.confirm_remove_title_named', {
          name: pendingRemoveName || $t('troubleshooting.unpair_single_unknown'),
        })
      "
      :message="
        $t('clients.confirm_remove_message_named', {
          name: pendingRemoveName || $t('troubleshooting.unpair_single_unknown'),
        })
      "
      :confirm-text="$t('clients.remove')"
      :cancelText="$t('cancel')"
      confirm-icon="fas fa-user-slash"
      variant="danger"
      icon="fas fa-triangle-exclamation"
      @confirm="confirmRemove"
      @cancel="showConfirmRemove = false"
    />

    <!-- Confirm unpair all -->
    <UiConfirmModal
      v-model:open="showConfirmUnpairAll"
      :title="$t('clients.confirm_unpair_all_title')"
      :message="$t('clients.confirm_unpair_all_message_count', { count: clients.length })"
      :confirm-text="$t('troubleshooting.unpair_all')"
      :cancelText="$t('cancel')"
      confirm-icon="fas fa-user-slash"
      variant="danger"
      icon="fas fa-triangle-exclamation"
      @confirm="confirmUnpairAll"
      @cancel="showConfirmUnpairAll = false"
    />
  </div>
</template>

<script setup>
import { ref, onMounted, computed } from 'vue';
import { http } from '@/http';
import UiCard from '@/components/UiCard.vue';
import UiButton from '@/components/UiButton.vue';
import UiAlert from '@/components/UiAlert.vue';
import ApiTokenManager from '@/ApiTokenManager.vue';
import UiConfirmModal from '@/components/UiConfirmModal.vue';

const clients = ref([]);
const pin = ref('');
const deviceName = ref('');
const pairing = ref(false);
const pairStatus = ref(null); // true/false/null
const unpairAllPressed = ref(false);
const unpairAllStatus = ref(null);
const removing = ref({});
const showConfirmRemove = ref(false);
const pendingRemoveUuid = ref('');
const pendingRemoveName = ref('');
const showConfirmUnpairAll = ref(false);

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

async function registerDevice() {
  if (pairing.value) return;
  pairStatus.value = null;
  pairing.value = true;
  try {
    const trimmedName = deviceName.value.trim();
    const body = { pin: pin.value.trim(), name: trimmedName };
    const r = await http.post('./api/pin', body, { validateStatus: () => true });
    const ok =
      r &&
      r.status >= 200 &&
      r.status < 300 &&
      (r.data?.status === true || r.data?.status === 'true' || r.data?.status === 1);
    pairStatus.value = !!ok;
    if (ok) {
      const prevCount = clients.value?.length || 0;
      // Kick one immediate refresh
      await refreshClients();
      // Poll briefly to catch eventual consistency from backend
      const deadline = Date.now() + 5000; // up to 5s
      const target = trimmedName.toLowerCase();
      while (Date.now() < deadline) {
        const found = clients.value?.some((c) => (c.name || '').toLowerCase() === target);
        if (found || (clients.value?.length || 0) > prevCount) break;
        await new Promise((res) => setTimeout(res, 400));
        await refreshClients();
      }
      // Clear inputs after we tried to load the updated list
      pin.value = '';
      deviceName.value = '';
    }
  } catch {
    pairStatus.value = false;
  } finally {
    pairing.value = false;
    setTimeout(() => {
      pairStatus.value = null;
    }, 5000);
  }
}

function askConfirmUnpair(uuid) {
  pendingRemoveUuid.value = uuid;
  const c = clients.value.find((x) => x.uuid === uuid);
  pendingRemoveName.value = c && c.name ? c.name : '';
  showConfirmRemove.value = true;
}

async function confirmRemove() {
  const uuid = pendingRemoveUuid.value;
  showConfirmRemove.value = false;
  pendingRemoveUuid.value = '';
  pendingRemoveName.value = '';
  if (!uuid) return;
  await unpairSingle(uuid);
}

async function unpairSingle(uuid) {
  if (removing.value[uuid]) return;
  removing.value = { ...removing.value, [uuid]: true };
  try {
    await http.post('./api/clients/unpair', { uuid }, { validateStatus: () => true });
  } catch {
  } finally {
    delete removing.value[uuid];
    removing.value = { ...removing.value };
    refreshClients();
  }
}

function askConfirmUnpairAll() {
  showConfirmUnpairAll.value = true;
}

async function confirmUnpairAll() {
  showConfirmUnpairAll.value = false;
  await unpairAll();
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

onMounted(() => refreshClients());

// ---------------- API TOKEN MANAGEMENT LOGIC (migrated from ApiTokenManager) ----------------
const scopes = ref([{ path: '', methods: [] }]);
const tokenResult = ref('');
const displayedToken = ref('');
const tokenCopied = ref(false);
const tokens = ref([]);
const tokenFilter = ref('');
const debouncedFilter = ref('');
const sortField = ref('created_at');
const sortDir = ref('desc');
const copiedHash = ref('');
const isGenerating = ref(false);
const isLoadingTokens = ref(false);
const revoking = ref('');
const testPath = ref('');
const testTokenInput = ref('');
const testResult = ref('');
const testError = ref('');
const isTesting = ref(false);

const API_ROUTES = [
  { path: '/api/pin', methods: ['POST'] },
  { path: '/api/apps', methods: ['GET', 'POST'] },
  { path: '/api/logs', methods: ['GET'] },
  { path: '/api/config', methods: ['GET', 'POST'] },
  { path: '/api/configLocale', methods: ['GET'] },
  { path: '/api/restart', methods: ['POST'] },
  { path: '/api/reset-display-device-persistence', methods: ['POST'] },
  { path: '/api/password', methods: ['POST'] },
  { path: '/api/apps/([0-9]+)', methods: ['DELETE'] },
  { path: '/api/clients/unpair-all', methods: ['POST'] },
  { path: '/api/clients/list', methods: ['GET'] },
  { path: '/api/clients/unpair', methods: ['POST'] },
  { path: '/api/apps/close', methods: ['POST'] },
  { path: '/api/covers/upload', methods: ['POST'] },
  { path: '/api/token', methods: ['POST'] },
  { path: '/api/tokens', methods: ['GET'] },
  { path: '/api/token/([a-fA-F0-9]+)', methods: ['DELETE'] },
];
const apiRoutes = ref(API_ROUTES);

const validScopes = computed(() =>
  scopes.value.filter((s) => s.path && Array.isArray(s.methods) && s.methods.length > 0),
);
const isGenerateDisabled = computed(() => !validScopes.value.length || isGenerating.value);
const filteredTokens = computed(() => {
  const filter = (debouncedFilter.value || '').trim().toLowerCase();
  if (!filter) return tokens.value;
  return tokens.value.filter(
    (t) =>
      t.username.toLowerCase().includes(filter) ||
      t.hash.toLowerCase().includes(filter) ||
      (t.scopes || []).some((s) => s.path.toLowerCase().includes(filter)),
  );
});
const sortedTokens = computed(() => {
  const arr = [...filteredTokens.value];
  arr.sort((a, b) => {
    let av, bv;
    if (sortField.value === 'created_at') {
      av = a.created_at;
      bv = b.created_at;
    } else if (sortField.value === 'username') {
      av = a.username.toLowerCase();
      bv = b.username.toLowerCase();
    } else {
      av = a.hash.toLowerCase();
      bv = b.hash.toLowerCase();
    }
    if (av < bv) return sortDir.value === 'asc' ? -1 : 1;
    if (av > bv) return sortDir.value === 'asc' ? 1 : -1;
    return 0;
  });
  return arr;
});

function addScope() {
  scopes.value.push({ path: '', methods: [] });
}
function onScopePathChange(scope) {
  scope.methods = [];
}
function removeScope(idx) {
  if (scopes.value.length > 1) scopes.value.splice(idx, 1);
  else scopes.value[0] = { path: '', methods: [] };
}
function getMethodsForPath(path) {
  const found = apiRoutes.value.find((r) => r.path === path);
  return found ? found.methods : ['GET', 'POST', 'DELETE', 'PATCH', 'PUT'];
}
function resetForm() {
  scopes.value = [{ path: '', methods: [] }];
  tokenResult.value = '';
  displayedToken.value = '';
  tokenCopied.value = false;
}

async function generateToken() {
  const filtered = validScopes.value;
  if (!filtered.length) {
    alert(window?.app?.$t?.('auth.please_specify_scope') || 'Specify scope');
    return;
  }
  isGenerating.value = true;
  tokenCopied.value = false;
  try {
    const res = await http.post('/api/token', { scopes: filtered }, { validateStatus: () => true });
    const data = res.data || {};
    if (res.status === 200 && data.token) {
      tokenResult.value = data.token;
      displayedToken.value = data.token;
      await loadTokens();
    } else {
      tokenResult.value = 'Error: ' + (data.error || 'Failed');
      displayedToken.value = '';
    }
  } catch (e) {
    tokenResult.value = 'Request failed: ' + e.message;
    displayedToken.value = '';
  } finally {
    isGenerating.value = false;
  }
}

async function loadTokens() {
  isLoadingTokens.value = true;
  try {
    const res = await http.get('/api/tokens', { validateStatus: () => true });
    if (res.status === 200) tokens.value = res.data || [];
    else tokens.value = [];
  } catch {
    tokens.value = [];
  } finally {
    isLoadingTokens.value = false;
  }
}

async function revokeToken(hash) {
  if (!confirm(window?.app?.$t?.('auth.confirm_revoke') || 'Revoke token?')) return;
  revoking.value = hash;
  try {
    const res = await http.delete(`/api/token/${hash}`, { validateStatus: () => true });
    if (res.status === 200) await loadTokens();
    else alert(window?.app?.$t?.('auth.failed_to_revoke_token') || 'Failed');
  } catch (e) {
    alert('Error: ' + e.message);
  } finally {
    revoking.value = '';
  }
}

function formatDate(ts) {
  const d = new Date(ts * 1000);
  return (
    d.toLocaleDateString() + ' ' + d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
  );
}
function formatFullDate(ts) {
  return new Date(ts * 1000).toLocaleString();
}

async function testToken() {
  testResult.value = '';
  testError.value = '';
  if (!testPath.value || !testTokenInput.value) {
    testError.value = 'Select path and token';
    return;
  }
  isTesting.value = true;
  try {
    const res = await http.get(testPath.value, {
      headers: { Authorization: 'Bearer ' + testTokenInput.value },
      validateStatus: () => true,
      responseType: 'text',
      transformResponse: [(v) => v],
    });
    const contentType = res.headers
      ? res.headers['content-type'] || res.headers['Content-Type']
      : '';
    let bodyText = res.data;
    const isJson = contentType && contentType.indexOf('application/json') !== -1 && bodyText;
    if (isJson) {
      try {
        testResult.value = JSON.stringify(JSON.parse(bodyText), null, 2);
      } catch {
        testResult.value = bodyText;
      }
    } else testResult.value = bodyText;
    if (res.status < 200 || res.status >= 300) testError.value = 'HTTP ' + res.status;
    else testError.value = '';
  } catch (e) {
    testError.value = 'Request failed: ' + e.message;
  } finally {
    isTesting.value = false;
  }
}
function copyToken() {
  if (!displayedToken.value) return;
  navigator.clipboard?.writeText(displayedToken.value).then(() => {
    tokenCopied.value = true;
    setTimeout(() => {
      tokenCopied.value = false;
    }, 3000);
  });
}
let _filterTimer;
function onFilterInput() {
  clearTimeout(_filterTimer);
  _filterTimer = setTimeout(() => {
    debouncedFilter.value = tokenFilter.value;
  }, 180);
}
function copyHash(hash) {
  navigator.clipboard?.writeText(hash).then(() => {
    copiedHash.value = hash;
    setTimeout(() => {
      if (copiedHash.value === hash) copiedHash.value = '';
    }, 2000);
  });
}

onMounted(() => {
  loadTokens();
});
</script>

<style scoped></style>
