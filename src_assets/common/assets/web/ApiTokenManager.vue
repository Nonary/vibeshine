<template>
  <div class="max-w-6xl mx-auto px-4 py-6 space-y-8">
    <header class="flex items-center justify-between gap-4">
      <div>
        <h1 class="text-2xl font-semibold tracking-tight">API Token Management</h1>
        <p class="text-sm opacity-70">Create scoped tokens, manage active ones, and test safely.</p>
      </div>
      <div class="text-xs opacity-75">
        <span class="inline-flex items-center gap-2">
          <i class="fas fa-shield-halved"></i>
          Least-privilege scopes for better security
        </span>
      </div>
    </header>

    <!-- Create Token -->
    <section
      class="p-5 mb-8 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm"
    >
      <header class="flex items-center gap-3 mb-4">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-key icon" /> Create Token
        </h2>
      </header>
      <div class="space-y-5">
        <div class="text-sm opacity-75">
          Choose one or more route scopes. Each scope grants specific HTTP methods for a path.
        </div>

        <!-- Add scope row -->
        <div class="flex flex-wrap items-start gap-3">
          <div class="flex flex-col flex-1 min-w-[240px]">
            <label class="form-label">Route</label>
            <select v-model="draft.path" class="form-control form-select">
              <option disabled value="">Select a route…</option>
              <option v-for="r in ROUTE_OPTIONS" :key="r.path" :value="r.path">
                {{ r.path }}
              </option>
            </select>
          </div>
          <div class="flex flex-col min-w-[220px]">
            <label class="form-label">Methods</label>
            <div class="flex flex-wrap gap-2">
              <label
                v-for="m in draftMethods"
                :key="m"
                class="inline-flex items-center gap-2 px-2 py-1 rounded-md bg-black/5 dark:bg-white/10 cursor-pointer select-none"
              >
                <input
                  v-model="draft.selectedMethods"
                  class="accent-primary"
                  type="checkbox"
                  :value="m"
                />
                <span class="uppercase text-[11px] tracking-wide font-semibold">{{ m }}</span>
              </label>
            </div>
            <div v-if="draft.path && draftMethods.length === 0" class="form-text">
              No methods available for this route.
            </div>
          </div>
          <div class="flex items-center">
            <UiButton variant="primary" size="md" :disabled="!canAddScope" @click="addScope">
              <i class="fas fa-plus icon" /> Add Scope
            </UiButton>
          </div>
        </div>

        <!-- Current scopes summary -->
        <div v-if="scopes.length" class="space-y-2">
          <div class="text-xs uppercase tracking-wider opacity-70">Scopes</div>
          <div class="flex flex-col gap-2">
            <div
              v-for="(s, idx) in scopes"
              :key="idx + ':' + s.path"
              class="flex items-start gap-2"
            >
              <div
                class="flex flex-wrap items-center gap-1 bg-black/5 dark:bg-white/10 rounded px-2 py-1"
              >
                <span class="font-semibold">{{ s.path }}</span>
                <span class="flex gap-1 flex-wrap">
                  <span
                    v-for="m in s.methods"
                    :key="m"
                    class="uppercase text-[11px] tracking-wide bg-primary/20 text-brand rounded px-1 py-0.5"
                    >{{ m }}</span
                  >
                </span>
              </div>
              <button class="btn ghost text-xs" title="Remove" @click="removeScope(idx)">
                <i class="fas fa-times icon"></i>
              </button>
            </div>
          </div>
        </div>

        <div class="flex items-center gap-3">
          <UiButton
            variant="success"
            size="md"
            :disabled="!canGenerate || creating"
            :loading="creating"
            @click="createToken"
          >
            <i class="fas fa-key icon" /> Generate Token
          </UiButton>
          <span v-if="creating" class="text-xs opacity-70">Creating…</span>
        </div>

        <!-- Created token output moved to modal -->

        <UiAlert v-if="createError" variant="danger" dismissible @close="createError = ''">
          {{ createError }}
        </UiAlert>
      </div>
    </section>

    <!-- Token Modal -->
    <UiModal v-model:open="showTokenModal" title="API Token Created" :dismissible="true">
      <div class="space-y-3">
        <UiAlert variant="warning">
          <i class="fas fa-triangle-exclamation" /> This token is shown only once. Save or copy it
          now. You cannot retrieve it later.
        </UiAlert>
        <div
          class="rounded-md border border-dark/10 dark:border-light/10 p-3 bg-black/5 dark:bg-white/10"
        >
          <div class="text-xs opacity-80">Token</div>
          <code class="block text-sm font-mono break-words my-1">{{ createdToken }}</code>
          <div class="flex items-center gap-3">
            <UiButton size="sm" variant="primary" @click="copy(createdToken)">
              <i class="fas fa-copy icon" /> Copy
            </UiButton>
            <span v-if="copied" class="text-xs text-success">Copied!</span>
          </div>
        </div>
      </div>
      <template #footer>
        <div class="flex items-center justify-end">
          <UiButton variant="primary" @click="showTokenModal = false">Done</UiButton>
        </div>
      </template>
    </UiModal>

    <!-- Active Tokens -->
    <section
      class="p-5 mb-8 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm"
    >
      <div class="flex items-center gap-3 mb-4">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-lock icon" /> Active Tokens
        </h2>
        <UiButton
          class="ml-auto"
          variant="neutral"
          tone="ghost"
          size="sm"
          :loading="tokensLoading"
          aria-label="Refresh tokens"
          @click="loadTokens"
        >
          <i class="fas fa-rotate icon" />
          <span class="hidden sm:inline">Refresh</span>
        </UiButton>
      </div>
      <div class="space-y-3">
        <div class="flex flex-wrap items-end gap-3">
          <div class="flex-1 min-w-[220px]">
            <input
              v-model="filter"
              class="form-control"
              placeholder="Filter by hash or path…"
              type="text"
            />
          </div>
          <div class="text-xs">
            <label class="flex flex-col">
              <span class="opacity-70">Sort</span>
              <select v-model="sortBy" class="form-control form-select">
                <option value="created">Newest</option>
                <option value="path">Path</option>
              </select>
            </label>
          </div>
        </div>

        <div v-if="tokensError" class="text-sm text-danger">{{ tokensError }}</div>

        <div v-if="filteredTokens.length === 0 && !tokensLoading" class="text-sm opacity-70">
          No active tokens.
        </div>

        <div v-else class="overflow-auto">
          <table class="table min-w-[640px]">
            <thead>
              <tr>
                <th class="w-40">Hash</th>
                <th>Scopes</th>
                <th class="w-28">Created</th>
                <th class="w-24"></th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="t in filteredTokens" :key="t.hash">
                <td class="align-middle">
                  <span
                    class="font-mono text-xs inline-block min-w-[70px] cursor-pointer"
                    tabindex="0"
                    title="Click to copy"
                    @click="copy(t.hash)"
                    @keydown.enter.prevent="copy(t.hash)"
                  >
                    {{ shortHash(t.hash) }}
                  </span>
                </td>
                <td class="align-middle">
                  <div class="flex flex-wrap gap-1">
                    <div
                      v-for="(s, idx) in t.scopes"
                      :key="idx"
                      class="rounded bg-black/5 dark:bg-white/10 px-2 py-1"
                    >
                      <span class="font-semibold">{{ s.path }}</span>
                      <span class="ml-1 space-x-1">
                        <span
                          v-for="m in s.methods"
                          :key="m"
                          class="uppercase text-[11px] tracking-wide bg-primary/20 text-brand rounded px-1 py-0.5"
                          >{{ m }}</span
                        >
                      </span>
                    </div>
                  </div>
                </td>
                <td class="text-xs opacity-70 align-middle">
                  {{ t.createdAt ? formatTime(t.createdAt) : '—' }}
                </td>
                <td class="align-middle">
                  <UiButton
                    size="sm"
                    variant="danger"
                    :loading="revoking === t.hash"
                    @click="promptRevoke(t)"
                  >
                    <i class="fas fa-ban icon" /> Revoke
                  </UiButton>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </section>

    <!-- Tester (GET only) -->
    <section
      class="p-5 mb-8 rounded-md border border-dark/10 dark:border-light/10 bg-white dark:bg-surface shadow-sm"
    >
      <header class="flex items-center gap-3 mb-4">
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-vial icon" /> Test Token
        </h2>
      </header>
      <div class="space-y-4">
        <UiAlert variant="neutral">
          <span class="text-xs">
            Tester performs only safe GET requests. Select a route and send a request with your
            token.
          </span>
        </UiAlert>
        <div class="grid md:grid-cols-2 gap-4">
          <div>
            <label class="form-label">Token</label>
            <input v-model="test.token" class="form-control monospace" placeholder="Paste token" />
          </div>
          <div>
            <label class="form-label">Route (GET only)</label>
            <select v-model="test.path" class="form-control form-select">
              <option disabled value="">Select a GET route…</option>
              <option v-for="r in GET_OPTIONS" :key="r.path" :value="r.path">{{ r.path }}</option>
            </select>
          </div>
          <div>
            <label class="form-label">Header Scheme</label>
            <select v-model="test.scheme" class="form-control form-select">
              <option value="bearer">Authorization: Bearer &lt;token&gt;</option>
              <option value="x-api-token">X-Api-Token: &lt;token&gt;</option>
              <option value="x-token">X-Token: &lt;token&gt;</option>
              <option value="query">Query param ?token=&lt;token&gt;</option>
            </select>
          </div>
          <div>
            <label class="form-label">Query String (optional)</label>
            <input
              v-model="test.query"
              class="form-control"
              placeholder="e.g. limit=50&tail=true"
            />
          </div>
        </div>
        <div class="flex items-center gap-3">
          <UiButton variant="primary" :disabled="!canSendTest" :loading="testing" @click="sendTest">
            <i class="fas fa-paper-plane icon" /> Test Token
          </UiButton>
          <span v-if="testing" class="text-xs opacity-70">Sending…</span>
        </div>

        <div v-if="testError" class="alert alert-danger text-sm">{{ testError }}</div>

        <div v-if="testResponse" class="space-y-2">
          <div class="text-xs opacity-70">Response</div>
          <pre
            class="p-3 rounded-md bg-black/5 dark:bg-white/10 text-dark dark:text-light text-xs overflow-auto max-h-[60vh]"
          ><code class="whitespace-pre-wrap">{{ testResponse }}</code></pre>
        </div>
      </div>
    </section>

    <!-- Revoke confirm modal -->
    <UiConfirmModal
      v-model:open="showRevoke"
      title="Revoke Token"
      :message="`Are you sure you want to revoke token ${shortHash(pendingRevoke?.hash || '')}? This cannot be undone.`"
      confirm-text="Revoke"
      confirm-icon="fas fa-ban"
      variant="danger"
      icon="fas fa-triangle-exclamation"
      @confirm="confirmRevoke"
      @cancel="showRevoke = false"
    />
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue';
import UiButton from '@/components/UiButton.vue';
import UiAlert from '@/components/UiAlert.vue';
import UiModal from '@/components/UiModal.vue';
import UiConfirmModal from '@/components/UiConfirmModal.vue';
import { http } from '@/http';

type RouteDef = { path: string; methods: string[] };
type Scope = { path: string; methods: string[] };
type TokenRecord = { hash: string; scopes: Scope[]; createdAt?: string | number | null };

// Available API routes and methods
const ROUTE_OPTIONS: RouteDef[] = [
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

const GET_OPTIONS = computed(() => ROUTE_OPTIONS.filter((r) => r.methods.includes('GET')));

// Create token state
const draft = reactive<{ path: string; selectedMethods: string[] }>({
  path: '',
  selectedMethods: [],
});
const scopes = ref<Scope[]>([]);
const creating = ref(false);
const createdToken = ref('');
const createError = ref('');
const copied = ref(false);
const showTokenModal = ref(false);

const draftMethods = computed<string[]>(
  () => ROUTE_OPTIONS.find((r) => r.path === draft.path)?.methods || [],
);
const canAddScope = computed(() => !!draft.path && draft.selectedMethods.length > 0);
const canGenerate = computed(
  () => scopes.value.length > 0 || (draft.path && draft.selectedMethods.length > 0),
);

function addScope(): void {
  if (!canAddScope.value) return;
  const methods = Array.from(new Set(draft.selectedMethods.map((m) => m.toUpperCase())));
  const existingIdx = scopes.value.findIndex((s) => s.path === draft.path);
  if (existingIdx !== -1) {
    // Merge and de-duplicate
    const merged = Array.from(
      new Set([...scopes.value[existingIdx].methods, ...methods]),
    ) as string[];
    scopes.value[existingIdx] = { path: draft.path, methods: merged };
  } else {
    scopes.value.push({ path: draft.path, methods });
  }
  // reset draft
  draft.path = '';
  draft.selectedMethods = [];
}

function removeScope(idx: number): void {
  scopes.value.splice(idx, 1);
}

function getEffectiveScopes(): Scope[] {
  const s = scopes.value.slice();
  if (draft.path && draft.selectedMethods.length > 0) {
    const methods = Array.from(new Set(draft.selectedMethods.map((m) => m.toUpperCase())));
    const idx = s.findIndex((x) => x.path === draft.path);
    if (idx !== -1) {
      s[idx] = {
        path: draft.path,
        methods: Array.from(new Set([...(s[idx].methods || []), ...methods])),
      };
    } else {
      s.push({ path: draft.path, methods });
    }
  }
  return s;
}

async function createToken(): Promise<void> {
  createError.value = '';
  createdToken.value = '';
  copied.value = false;
  creating.value = true;
  try {
    // Tentative payload; backend can map to expected format
    const payload = { scopes: getEffectiveScopes() };
    const res = await http.post('/api/token', payload, { validateStatus: () => true });
    if (res.status >= 200 && res.status < 300) {
      const token = (res.data && (res.data.token || res.data.value || res.data)) as string;
      if (typeof token === 'string' && token.length > 0) {
        createdToken.value = token;
        // refresh active list
        await loadTokens();
        showTokenModal.value = true;
      } else {
        createError.value = 'Token created, but server returned no token string.';
      }
    } else {
      const msg = (res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`;
      createError.value = `Failed to create token: ${msg}`;
    }
  } catch (e: any) {
    createError.value = e?.message || 'Network error creating token.';
  } finally {
    creating.value = false;
  }
}

async function copy(text: string): Promise<void> {
  copied.value = false;
  try {
    await navigator.clipboard.writeText(text);
    copied.value = true;
    setTimeout(() => (copied.value = false), 1500);
  } catch {
    // fallback
    try {
      const ta = document.createElement('textarea');
      ta.value = text;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
      copied.value = true;
      setTimeout(() => (copied.value = false), 1500);
    } catch {
      /* noop */
    }
  }
}

// Active tokens state
const tokens = ref<TokenRecord[]>([]);
const tokensLoading = ref(false);
const tokensError = ref('');
const filter = ref('');
const sortBy = ref<'created' | 'path'>('created');
const revoking = ref('');
const showRevoke = ref(false);
const pendingRevoke = ref<TokenRecord | null>(null);

function normalizeToken(rec: any): TokenRecord | null {
  if (!rec) return null;
  const scopes: Scope[] = Array.isArray(rec.scopes)
    ? rec.scopes.map((s: any) => ({
        path: s.path || s.route || '',
        methods: (s.methods || s.verbs || []).map((v: any) => String(v).toUpperCase()),
      }))
    : [];
  const hash: string = rec.hash || rec.id || rec.token_hash || '';
  const createdAt = rec.createdAt || rec.created_at || rec.created || null;
  if (!hash) return null;
  return { hash, scopes, createdAt };
}

async function loadTokens(): Promise<void> {
  tokensLoading.value = true;
  tokensError.value = '';
  try {
    const res = await http.get('/api/tokens', { validateStatus: () => true });
    if (res.status >= 200 && res.status < 300) {
      const list = Array.isArray(res.data) ? res.data : res.data?.tokens || [];
      tokens.value = (list as any[]).map((x) => normalizeToken(x)).filter(Boolean) as TokenRecord[];
    } else {
      const msg = (res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`;
      tokensError.value = `Failed to load tokens: ${msg}`;
    }
  } catch (e: any) {
    tokensError.value = e?.message || 'Network error loading tokens.';
  } finally {
    tokensLoading.value = false;
  }
}

function promptRevoke(t: TokenRecord): void {
  pendingRevoke.value = t;
  showRevoke.value = true;
}

async function confirmRevoke(): Promise<void> {
  const t = pendingRevoke.value;
  if (!t?.hash) return;
  revoking.value = t.hash;
  try {
    const url = `/api/token/${encodeURIComponent(t.hash)}`;
    const res = await http.delete(url, { validateStatus: () => true });
    if (res.status >= 200 && res.status < 300) {
      tokens.value = tokens.value.filter((x) => x.hash !== t.hash);
      showRevoke.value = false;
      pendingRevoke.value = null;
    } else {
      const msg = (res.data && (res.data.message || res.data.error)) || `HTTP ${res.status}`;
      alert(`Failed to revoke: ${msg}`);
    }
  } catch (e: any) {
    alert(`Failed to revoke: ${e?.message || 'Network error'}`);
  } finally {
    revoking.value = '';
  }
}

const filteredTokens = computed<TokenRecord[]>(() => {
  const q = (filter.value || '').toLowerCase();
  let out = tokens.value.filter((t) => {
    if (!q) return true;
    if (t.hash.toLowerCase().includes(q)) return true;
    return t.scopes.some((s) => s.path.toLowerCase().includes(q));
  });
  if (sortBy.value === 'created') {
    out = out
      .slice()
      .sort((a, b) => String(b.createdAt || '').localeCompare(String(a.createdAt || '')));
  } else if (sortBy.value === 'path') {
    out = out
      .slice()
      .sort((a, b) => (a.scopes[0]?.path || '').localeCompare(b.scopes[0]?.path || ''));
  }
  return out;
});

function shortHash(h: string): string {
  if (!h) return '';
  if (h.length <= 10) return h;
  return `${h.slice(0, 6)}…${h.slice(-4)}`;
}

function formatTime(v: any): string {
  try {
    const d = typeof v === 'number' ? new Date(v) : new Date(String(v));
    if (isNaN(d.getTime())) return '—';
    return d.toLocaleString();
  } catch {
    return '—';
  }
}

// Tester state
const test = reactive<{
  token: string;
  path: string;
  query: string;
  scheme: 'bearer' | 'x-api-token' | 'x-token' | 'query';
}>({ token: '', path: '', query: '', scheme: 'bearer' });
const testing = ref(false);
const testResponse = ref('');
const testError = ref('');
const canSendTest = computed(() => !!test.token && !!test.path);

async function sendTest(): Promise<void> {
  testError.value = '';
  testResponse.value = '';
  testing.value = true;
  try {
    const urlBase = test.path;
    const qs = (test.query || '').trim();
    const fullUrl = qs ? `${urlBase}?${qs}` : urlBase;
    const headers: Record<string, string> = { 'X-Requested-With': 'XMLHttpRequest' };
    if (test.scheme === 'bearer') headers['Authorization'] = `Bearer ${test.token}`;
    if (test.scheme === 'x-api-token') headers['X-Api-Token'] = test.token;
    if (test.scheme === 'x-token') headers['X-Token'] = test.token;
    const url =
      test.scheme === 'query'
        ? `${fullUrl}${fullUrl.includes('?') ? '&' : '?'}token=${encodeURIComponent(test.token)}`
        : fullUrl;
    const res = await http.get(url, { headers, validateStatus: () => true });
    const pretty = prettyPrint(res.data);
    testResponse.value = `${res.status} ${res.statusText || ''}\n\n${pretty}`;
  } catch (e: any) {
    testError.value = e?.message || 'Test request failed.';
  } finally {
    testing.value = false;
  }
}

function prettyPrint(data: any): string {
  try {
    if (typeof data === 'string') {
      // try parse as JSON, else return as-is
      try {
        const obj = JSON.parse(data);
        return JSON.stringify(obj, null, 2);
      } catch {
        return data;
      }
    }
    return JSON.stringify(data, null, 2);
  } catch {
    return String(data);
  }
}

onMounted(async () => {
  await loadTokens();
  // Auto-fill tester token after create
  watchCreatedForTester();
});

function watchCreatedForTester() {
  let last = '';
  const iv = setInterval(() => {
    if (createdToken.value && createdToken.value !== last) {
      test.token = createdToken.value;
      last = createdToken.value;
    }
  }, 300);
  // Stop after some time to avoid leaks
  setTimeout(() => clearInterval(iv), 30000);
}
</script>

<style scoped>
.icon {
  font-size: 0.95em;
  line-height: 1;
}

.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.2s ease;
}
.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}
</style>
