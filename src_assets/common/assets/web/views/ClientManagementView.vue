<template>
  <div class="mx-auto max-w-4xl px-4">
    <h1 class="text-2xl font-semibold my-6 flex items-center gap-3 text-brand">
      <i class="fas fa-users-cog" /> {{ $t('clients.title') }}
    </h1>

    <!-- Pair New Client -->
    <section
      class="p-5 mb-8 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm space-y-3"
    >
      <header class="space-y-1">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-link" /> {{ $t('clients.pair_title') }}
        </h2>
        <p class="text-sm opacity-75">{{ $t('clients.pair_desc') }}</p>
      </header>
      <form class="flex flex-col gap-4 md:flex-row md:items-end" @submit.prevent="registerDevice">
        <div class="flex flex-col flex-1">
          <label class="text-[11px] font-semibold tracking-wide uppercase mb-1" for="pin-input">{{
            $t('navbar.pin')
          }}</label>
          <input
            id="pin-input"
            v-model="pin"
            type="text"
            inputmode="numeric"
            pattern="\\d*"
            maxlength="4"
            class="form-control"
            :placeholder="$t('navbar.pin')"
            required
          />
        </div>
        <div class="flex flex-col flex-1">
          <label class="text-[11px] font-semibold tracking-wide uppercase mb-1" for="name-input">{{
            $t('pin.device_name')
          }}</label>
          <input
            id="name-input"
            v-model="deviceName"
            type="text"
            class="form-control"
            :placeholder="$t('pin.device_name')"
            required
          />
        </div>
        <div class="flex flex-col">
          <UiButton :disabled="pairing" class="mt-1 md:mt-0" variant="primary" type="submit">
            <span v-if="!pairing">{{ $t('pin.send') }}</span>
            <span v-else>{{ $t('clients.pairing') }}</span>
          </UiButton>
        </div>
      </form>
      <div>
        <UiAlert v-if="pairStatus === true" variant="success">{{ $t('pin.pair_success') }}</UiAlert>
        <UiAlert v-if="pairStatus === false" variant="danger">{{ $t('pin.pair_failure') }}</UiAlert>
      </div>
      <UiAlert variant="warning" class="text-sm flex items-start gap-2">
        <template #default>
          <div class="flex items-start gap-2">
            <b class="font-semibold">{{ $t('_common.warning') }}</b>
            <span>{{ $t('pin.warning_msg') }}</span>
          </div>
        </template>
      </UiAlert>
    </section>

    <!-- Existing Clients -->
    <section
      class="p-5 mb-8 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm"
    >
      <div class="flex items-center gap-3 mb-4">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-users" /> {{ $t('clients.existing_title') }}
        </h2>
        <UiButton
          class="ml-auto"
          variant="danger"
          :disabled="unpairAllPressed || clients.length === 0"
          @click="unpairAll"
        >
          <i class="fas fa-user-slash" /> {{ $t('troubleshooting.unpair_all') }}
        </UiButton>
      </div>
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
            @click="unpairSingle(client.uuid)"
          >
            <i class="fas fa-trash" />
          </UiButton>
        </li>
      </ul>
      <div v-else class="p-4 text-center italic opacity-75">
        {{ $t('troubleshooting.unpair_single_no_devices') }}
      </div>
    </section>

    <section
      class="p-5 mb-8 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm"
    >
      <h2 class="text-lg font-medium mb-4 flex items-center gap-2">
        <i class="fas fa-key" /> {{ $t('auth.generate_new_token') }}
      </h2>
      <p v-if="$te('auth.generate_token_help')" class="text-xs opacity-75 mb-3">
        {{ $t('auth.generate_token_help') }}
      </p>
      <form novalidate class="space-y-5" @submit.prevent="generateToken">
        <div class="space-y-4">
          <div
            v-for="(scope, idx) in scopes"
            :key="scope.id ?? idx"
            class="grid grid-cols-1 md:grid-cols-12 gap-4 items-end"
          >
            <div class="col-span-12 md:col-span-5 flex flex-col">
              <label :for="'scope-path-' + idx" class="text-[11px] font-semibold uppercase mb-1">{{
                $t('auth.select_api_path')
              }}</label>
              <select
                :id="'scope-path-' + idx"
                v-model="scope.path"
                class="form-control"
                @change="onScopePathChange(scope)"
              >
                <option value="" disabled>{{ $t('auth.select_api_path') }}</option>
                <option
                  v-for="route in apiRoutes.filter((r) => r.selectable !== false)"
                  :key="route.path"
                  :value="route.path"
                >
                  {{ route.path }}
                </option>
              </select>
            </div>
            <div class="col-span-12 md:col-span-5 flex flex-col">
              <label
                :for="'scope-methods-' + idx"
                class="text-[11px] font-semibold uppercase mb-1"
                >{{ $t('auth.scopes') }}</label
              >
              <select
                :id="'scope-methods-' + idx"
                v-model="scope.methods"
                multiple
                size="4"
                :disabled="!scope.path"
                class="form-control h-28"
              >
                <option v-for="m in getMethodsForPath(scope.path)" :key="m" :value="m">
                  {{ m }}
                </option>
              </select>
            </div>
            <div class="col-span-12 md:col-span-2 flex">
              <UiButton
                type="button"
                variant="danger"
                class="w-full"
                size="sm"
                :aria-label="$t('auth.remove')"
                :disabled="scopes.length === 1 && !scope.path && !scope.methods?.length"
                @click="removeScope(idx)"
              >
                {{ $t('auth.remove') }}
              </UiButton>
            </div>
          </div>

          <div class="flex items-center gap-4 flex-wrap">
            <UiButton type="button" variant="primary" size="sm" @click="addScope">
              {{ $t('auth.add_scope') }}
            </UiButton>
            <span v-if="isGenerateDisabled" class="text-xs opacity-60">{{
              $t('auth.generate_disabled_hint')
            }}</span>
          </div>

          <div v-if="validScopes.length" class="pt-1">
            <strong class="mr-2 text-sm">{{ $t('auth.selected_scopes') }}:</strong>
            <div class="flex flex-wrap items-center gap-2 mt-2">
              <template v-for="(s, i) in validScopes" :key="i">
                <span
                  class="inline-flex items-center bg-dark/5 dark:bg-light/10 text-xs rounded px-2 py-1"
                >
                  <span class="font-semibold mr-2">{{ s.path }}</span>
                  <span
                    v-for="m in s.methods"
                    :key="m"
                    class="ml-1 inline-flex items-center bg-primary text-onPrimary text-[10px] uppercase rounded px-1.5 py-0.5"
                    >{{ m }}</span
                  >
                </span>
              </template>
            </div>
          </div>

          <div class="flex gap-3 flex-wrap">
            <UiButton
              type="submit"
              variant="primary"
              :disabled="isGenerateDisabled || isGenerating"
              :loading="isGenerating"
            >
              <span v-if="!isGenerating">{{ $t('auth.generate_token') }}</span>
              <span v-else>{{ $t('auth.loading') }}</span>
            </UiButton>
            <UiButton
              type="button"
              tone="ghost"
              variant="neutral"
              :disabled="isGenerating"
              @click="resetForm"
            >
              {{ $t('_common.cancel') }}
            </UiButton>
          </div>

          <UiAlert v-if="displayedToken" variant="success" class="mt-2">
            <div class="mb-2 font-medium text-sm">{{ $t('auth.token_success') }}</div>
            <div class="flex gap-2 flex-col md:flex-row">
              <input
                type="text"
                class="form-control flex-1 font-mono"
                :value="displayedToken"
                readonly
              />
              <UiButton
                type="button"
                variant="primary"
                :disabled="tokenCopied"
                :title="$t('auth.copy_token')"
                @click="copyToken"
              >
                {{ tokenCopied ? $t('auth.token_copied') : $t('auth.copy_token') }}
              </UiButton>
            </div>
          </UiAlert>
        </div>
      </form>
    </section>

    <!-- API Token Management: Active Tokens -->
    <section
      class="p-5 mb-8 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm"
    >
      <div class="flex items-center gap-3 mb-5 flex-wrap">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-list" /> {{ $t('auth.active_tokens') }}
        </h2>
        <UiButton
          type="button"
          tone="ghost"
          variant="neutral"
          size="sm"
          :disabled="isLoadingTokens"
          @click="loadTokens"
        >
          {{ $t('auth.refresh') }}
        </UiButton>
        <div class="ml-auto flex gap-4 flex-wrap items-end">
          <div class="flex flex-col w-40">
            <label class="text-[11px] font-semibold uppercase mb-1">{{
              $t('auth.search_tokens')
            }}</label>
            <input
              v-model="tokenFilter"
              type="text"
              class="form-control"
              :placeholder="$t('auth.search_tokens')"
              autocomplete="off"
              @input="onFilterInput"
            />
          </div>
          <div class="flex flex-col w-40">
            <label class="text-[11px] font-semibold uppercase mb-1">{{
              $t('auth.sort_field')
            }}</label>
            <select v-model="sortField" class="form-control">
              <option value="created_at">{{ $t('auth.created') }}</option>
              <option value="username">{{ $t('auth.username') }}</option>
              <option value="hash">{{ $t('auth.hash') }}</option>
            </select>
          </div>
          <div class="flex flex-col w-32">
            <label class="text-[11px] font-semibold uppercase mb-1">{{
              $t('auth.sort_direction')
            }}</label>
            <select v-model="sortDir" class="form-control">
              <option value="desc">{{ $t('auth.desc') }}</option>
              <option value="asc">{{ $t('auth.asc') }}</option>
            </select>
          </div>
        </div>
      </div>
      <div class="overflow-x-auto">
        <table class="w-full text-sm">
          <thead class="bg-dark/5 dark:bg-light/5">
            <tr class="text-left">
              <th class="p-2 font-semibold">{{ $t('auth.hash') }}</th>
              <th class="p-2 font-semibold">{{ $t('auth.username') }}</th>
              <th class="p-2 font-semibold">{{ $t('auth.created') }}</th>
              <th class="p-2 font-semibold">{{ $t('auth.scopes') }}</th>
              <th class="p-2 text-right" />
            </tr>
          </thead>
          <tbody class="divide-y divide-dark/10 dark:divide-light/10">
            <tr v-if="isLoadingTokens">
              <td colspan="5" class="text-center py-6 text-sm">{{ $t('auth.loading') }}</td>
            </tr>
            <tr v-else-if="!sortedTokens.length">
              <td colspan="5" class="text-center py-6 text-sm">
                {{ tokens.length ? $t('auth.no_matching_tokens') : $t('auth.no_active_tokens') }}
              </td>
            </tr>
            <tr v-for="t in sortedTokens" :key="t.hash" class="hover:bg-primary/5 transition">
              <td class="p-2 align-top max-w-[160px]">
                <div class="flex items-center gap-2">
                  <code class="truncate text-[11px] font-mono" :title="t.hash">{{ t.hash }}</code>
                  <UiButton
                    type="button"
                    tone="ghost"
                    variant="neutral"
                    size="sm"
                    :disabled="copiedHash === t.hash"
                    @click.prevent="copyHash(t.hash)"
                  >
                    {{ copiedHash === t.hash ? $t('auth.hash_copied') : $t('auth.copy_hash') }}
                  </UiButton>
                </div>
              </td>
              <td class="p-2 align-top max-w-[160px] truncate">{{ t.username }}</td>
              <td class="p-2 align-top" :title="formatFullDate(t.created_at)">
                {{ formatDate(t.created_at) }}
              </td>
              <td class="p-2 align-top">
                <div class="flex flex-wrap gap-1">
                  <span v-for="(s, i) in t.scopes" :key="i" class="inline-flex items-start">
                    <span
                      class="inline-flex items-center bg-dark/5 dark:bg-light/10 text-xs rounded px-2 py-0.5 mr-1"
                      >{{ s.path }}</span
                    >
                    <span
                      v-for="m in s.methods"
                      :key="m"
                      class="inline-flex items-center bg-primary text-onPrimary text-[10px] uppercase rounded px-1.5 py-0.5 mr-1"
                      >{{ m }}</span
                    >
                  </span>
                </div>
              </td>
              <td class="p-2 align-top text-right">
                <UiButton
                  variant="danger"
                  size="sm"
                  :disabled="revoking === t.hash"
                  @click="revokeToken(t.hash)"
                >
                  {{ $t('auth.revoke') }}
                </UiButton>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- API Token Management: Test Token -->
    <section
      class="p-5 mb-14 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm"
    >
      <h2 class="text-lg font-medium mb-4 flex items-center gap-2">
        <i class="fas fa-vial" /> {{ $t('auth.test_api_token') }}
      </h2>
      <p v-if="$te('auth.testing_help')" class="text-xs opacity-75 mb-4">
        {{ $t('auth.testing_help') }}
      </p>
      <form class="grid grid-cols-1 md:grid-cols-12 gap-4 mb-4" @submit.prevent="testToken">
        <div class="col-span-1 md:col-span-6 flex flex-col">
          <label for="testPath" class="text-[11px] font-semibold uppercase mb-1">{{
            $t('auth.api_path_get_only')
          }}</label>
          <select id="testPath" v-model="testPath" class="form-control" required>
            <option value="" disabled>{{ $t('auth.select_api_path_to_test') }}</option>
            <option
              v-for="route in apiRoutes.filter(
                (r) => r.selectable !== false && r.methods.includes('GET'),
              )"
              :key="route.path"
              :value="route.path"
            >
              {{ route.path }}
            </option>
          </select>
        </div>
        <div class="col-span-1 md:col-span-6 flex flex-col">
          <label for="testTokenInput" class="text-[11px] font-semibold uppercase mb-1">{{
            $t('auth.token')
          }}</label>
          <input
            id="testTokenInput"
            v-model="testTokenInput"
            type="password"
            class="form-control"
            autocomplete="off"
            :placeholder="$t('auth.paste_token_here')"
            required
          />
        </div>
        <div class="col-span-1 flex items-end">
          <UiButton
            type="submit"
            variant="primary"
            :disabled="isTesting || !testPath || !testTokenInput"
            :loading="isTesting"
          >
            <span v-if="!isTesting">{{ $t('auth.test_token') }}</span>
            <span v-else>{{ $t('auth.loading') }}</span>
          </UiButton>
        </div>
      </form>
      <div v-if="testResult || testError" class="mt-2">
        <div class="font-semibold mb-2 text-sm">{{ $t('auth.result') }}</div>
        <UiAlert v-if="testError" variant="danger" class="mb-2">{{ testError }}</UiAlert>
        <pre
          v-if="testResult"
          class="bg-dark/5 dark:bg-light/5 p-3 rounded text-[11px] overflow-auto max-h-72 whitespace-pre-wrap font-mono"
          >{{ testResult }}</pre
        >
      </div>
    </section>
  </div>
</template>

<script setup>
import { ref, onMounted, computed } from 'vue';
import { http } from '@/http.js';
import UiButton from '@/components/UiButton.vue';
import UiAlert from '@/components/UiAlert.vue';

const clients = ref([]);
const pin = ref('');
const deviceName = ref('');
const pairing = ref(false);
const pairStatus = ref(null); // true/false/null
const unpairAllPressed = ref(false);
const unpairAllStatus = ref(null);
const removing = ref({});

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
    const body = { pin: pin.value.trim(), name: deviceName.value.trim() };
    const r = await http.post('./api/pin', body, { validateStatus: () => true });
    pairStatus.value = r.data?.status === true;
    if (pairStatus.value) {
      pin.value = '';
      deviceName.value = '';
      refreshClients();
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
