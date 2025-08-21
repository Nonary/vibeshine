<template>
  <div class="space-y-6">
    <n-alert v-if="platform && platform !== 'windows'" type="info" :show-icon="true">
      {{ $t('playnite.only_windows') }}
    </n-alert>

    <section v-if="platform === 'windows'" class="space-y-3">
      <h3 class="text-sm font-semibold uppercase tracking-wider">{{ $t('playnite.status_title') }}</h3>
      <div class="bg-light/70 dark:bg-surface/70 border border-dark/10 dark:border-light/10 rounded-lg p-4 space-y-4">
        <div>
          <Checkbox
            v-model="cfg.playnite_enabled"
            id="playnite_enabled"
            :default="store.defaults.playnite_enabled"
            :localePrefix="'playnite'"
            :label="$t('playnite.integration_enabled')"
            :desc="''"
          />
        </div>
        <div class="text-sm grid md:grid-cols-3 gap-3">
          <div class="flex items-center gap-2">
            <b>Status:</b>
            <n-tooltip v-if="statusKind === 'session'" trigger="hover">
              <template #trigger>
                <n-tag size="small" :type="statusType">{{ statusText }}</n-tag>
              </template>
              <span>{{ $t('playnite.session_required_tooltip') }}</span>
            </n-tooltip>
            <n-tag v-else size="small" :type="statusType">{{ statusText }}</n-tag>
          </div>
          <div class="flex items-center gap-2" v-if="status.extensions_dir">
            <b>{{ $t('playnite.extensions_dir') }}:</b>
            <code class="text-xs break-all">{{ status.extensions_dir }}</code>
          </div>
          <div class="flex items-center gap-2" v-if="status.plugin_version">
            <b>{{ $t('playnite.plugin_version') || 'Plugin' }}:</b>
            <n-tag size="small" type="default">v{{ status.plugin_version }}</n-tag>
            <template v-if="status.plugin_latest">
              <span class="opacity-70">→</span>
              <n-tag size="small" :type="pluginOutdated ? 'warning' : 'success'">v{{ status.plugin_latest }}</n-tag>
            </template>
          </div>
        </div>
        <n-alert v-if="statusKind === 'session'" type="warning" :show-icon="true">
          {{ $t('playnite.session_required') }}
        </n-alert>
        <n-alert v-if="pluginOutdated" type="warning" :show-icon="true">
          {{ $t('playnite.plugin_outdated', { installed: status.plugin_version || '?', latest: status.plugin_latest || '?' }) }}
        </n-alert>
        <div class="flex items-center gap-2">
          <n-button v-if="status.playnite_running !== false" type="primary" size="small" :loading="installing" @click="openInstallConfirm">
            <i class="fas fa-plug" />
            <span class="ml-2">
              {{
                status.installed
                  ? (pluginOutdated
                      ? ($t('playnite.upgrade_button') || 'Upgrade Plugin')
                      : ($t('playnite.reinstall_button') || 'Reinstall Plugin'))
                  : ($t('playnite.install_button') || 'Install Plugin')
              }}
            </span>
          </n-button>
          <span v-if="installOk" class="text-success text-xs">{{ $t('playnite.install_success') }}</span>
          <span v-if="installErr" class="text-danger text-xs">{{ $t('playnite.install_error') }}<template v-if="installErrorMsg">: {{ installErrorMsg }}</template></span>
        </div>
      </div>
    </section>

    <section v-if="platform === 'windows'" class="space-y-4">
      <h3 class="text-sm font-semibold uppercase tracking-wider">{{ $t('playnite.settings_title') }}</h3>
      <div class="grid md:grid-cols-3 gap-4">
        <div class="md:col-span-1">
          <Checkbox
            v-model="cfg.playnite_auto_sync"
            id="playnite_auto_sync"
            :default="store.defaults.playnite_auto_sync"
            :localePrefix="'playnite'"
          />
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.recent_games') }}</label>
          <n-input-number v-model:value="cfg.playnite_recent_games" :min="0" />
          <div class="text-[11px] opacity-70">{{ $t('playnite.recent_games_desc') }}</div>
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.recent_max_age_days') }}</label>
          <n-input-number v-model:value="cfg.playnite_recent_max_age_days" :min="0" />
          <div class="text-[11px] opacity-70">{{ $t('playnite.recent_max_age_days_desc') }}</div>
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.delete_after_days') }}</label>
          <n-input-number v-model:value="cfg.playnite_autosync_delete_after_days" :min="0" />
          <div class="text-[11px] opacity-70">{{ $t('playnite.delete_after_days_desc') }}</div>
        </div>
        <div class="md:col-span-2">
          <Checkbox
            v-model="cfg.playnite_autosync_require_replacement"
            id="playnite_autosync_require_replacement"
            :default="store.defaults.playnite_autosync_require_replacement"
            :localePrefix="'playnite'"
            :label="$t('playnite.require_replacement')"
            :desc="$t('playnite.require_replacement_desc')"
          />
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.focus_attempts') || 'Auto-focus attempts' }}</label>
          <n-input-number v-model:value="cfg.playnite_focus_attempts" :min="0" />
          <div class="text-[11px] opacity-70">{{ $t('playnite.focus_attempts_help') || 'Number of times to try to bring Playnite windows to the foreground when launching.' }}</div>
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.focus_timeout_secs') || 'Auto-focus timeout window (seconds)' }}</label>
          <n-input-number v-model:value="cfg.playnite_focus_timeout_secs" :min="0" />
          <div class="text-[11px] opacity-70">{{ $t('playnite.focus_timeout_secs_help') || 'How long auto-focus runs while re-applying focus (0 to disable).' }}</div>
        </div>
        <div class="md:col-span-1 flex items-end">
          <Checkbox
            v-model="cfg.playnite_focus_exit_on_first"
            id="playnite_focus_exit_on_first"
            :default="store.defaults.playnite_focus_exit_on_first"
            :localePrefix="'playnite'"
            :label="$t('playnite.focus_exit_on_first') || 'Exit on first confirmed focus'"
            :desc="$t('playnite.focus_exit_on_first_help') || 'Stop auto-focus after first success; otherwise, re-apply until attempts or timeout elapse.'"
          />
        </div>
      </div>

      <div class="grid md:grid-cols-2 gap-4">
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.sync_categories') }}</label>
          <n-select
            v-model:value="selectedCategories"
            multiple
            :options="categoryOptions"
            filterable
            tag
            :loading="categoriesLoading"
            @focus="loadCategories"
          />
          <div class="text-[11px] opacity-70">{{ $t('playnite.sync_categories_help') }}</div>
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.exclude_games') || 'Exclude games from auto-sync' }}</label>
          <n-select
            v-model:value="excludedIds"
            multiple
            :options="gameOptions"
            filterable
            :loading="gamesLoading"
            @focus="loadGames"
          />
          <div class="text-[11px] opacity-70">{{ $t('playnite.exclude_games_desc') || 'Selected games will not be auto-synced from Playnite.' }}</div>
        </div>
      </div>

      
    </section>
  </div>
  <!-- Install/Upgrade confirmation -->
  <n-modal :show="showInstallConfirm" @update:show="(v) => (showInstallConfirm = v)">
    <n-card :bordered="false" style="max-width: 32rem; width: 100%">
      <template #header>
        <div class="flex items-center gap-2">
          <i class="fas fa-plug" />
          <span>
            {{ status.installed ? (pluginOutdated ? ($t('playnite.upgrade_button') || 'Upgrade Plugin') : ($t('playnite.reinstall_button') || 'Reinstall Plugin')) : ($t('playnite.install_button') || 'Install Plugin') }}
          </span>
        </div>
      </template>
      <div class="text-sm">
        {{ $t('playnite.install_requires_restart') || 'This action will restart Playnite to complete plugin (re)installation. Continue?' }}
      </div>
      <template #footer>
        <div class="w-full flex items-center justify-center gap-3">
          <n-button tertiary @click="showInstallConfirm = false">{{ $t('_common.cancel') }}</n-button>
          <n-button type="primary" :loading="installing" @click="confirmInstall">{{ $t('_common.continue') || 'Continue' }}</n-button>
        </div>
      </template>
    </n-card>
  </n-modal>
</template>

<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue';
import { NInput, NInputNumber, NSelect, NButton, NAlert, NTag, NTooltip } from 'naive-ui';
import { useI18n } from 'vue-i18n';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { http } from '@/http';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const cfg = computed<any>(() => config.value || {});
const platform = computed(() => (metadata.value?.platform || cfg.value?.platform || '').toLowerCase());
const { t } = useI18n();

const status = reactive<{
  installed: boolean;
  installed_unknown?: boolean;
  active: boolean;
  user_session_active: boolean;
  playnite_running?: boolean;
  extensions_dir: string;
  plugin_version?: string;
  plugin_latest?: string;
}>({ installed: false, active: false, user_session_active: false, extensions_dir: '' });
const installing = ref(false);
const showInstallConfirm = ref(false);
const installOk = ref(false);
const installErr = ref(false);
const installErrorMsg = ref('');

const categoriesLoading = ref(false);
const gamesLoading = ref(false);
const categoryOptions = ref<{ label: string; value: string }[]>([]);
const gameOptions = ref<{ label: string; value: string }[]>([]);

const selectedCategories = computed<string[]>({
  get() {
    const raw = (cfg.value?.playnite_sync_categories || '') as string;
    return raw.split(';').map((s) => s.trim()).filter(Boolean);
  },
  set(v: string[]) {
    store.updateOption('playnite_sync_categories', (v || []).join(';'));
  },
});

const excludedIds = computed<string[]>({
  get() {
    const raw = (cfg.value?.playnite_exclude_games || '') as string;
    return raw.split(',').map((s) => s.trim()).filter(Boolean);
  },
  set(v: string[]) {
    store.updateOption('playnite_exclude_games', (v || []).join(','));
  },
});

async function refreshStatus() {
  if (platform.value !== 'windows') return;
  try {
    const r = await http.get('/api/playnite/status');
    if (r.status === 200 && r.data) {
      const d = r.data as any;
      status.installed = !!d.installed;
      status.active = !!d.active;
      status.user_session_active = !!d.user_session_active;
      status.extensions_dir = d.extensions_dir || '';
      status.plugin_version = d.plugin_version || d.version || status.plugin_version;
      status.plugin_latest = d.plugin_latest || d.latest_version || status.plugin_latest;
    }
  } catch (_) {}
}

async function loadCategories() {
  if (platform.value !== 'windows') return;
  if (categoriesLoading.value || categoryOptions.value.length) return;
  categoriesLoading.value = true;
  try {
    const r = await http.get('/api/playnite/games');
    const games: any[] = Array.isArray(r.data) ? r.data : [];
    const set = new Set<string>();
    for (const g of games) {
      for (const c of g?.categories || []) if (c && typeof c === 'string') set.add(c);
    }
    categoryOptions.value = Array.from(set).sort((a, b) => a.localeCompare(b)).map((c) => ({ label: c, value: c }));
  } catch (_) {}
  categoriesLoading.value = false;
}

async function loadGames() {
  if (platform.value !== 'windows') return;
  if (gamesLoading.value || gameOptions.value.length) return;
  gamesLoading.value = true;
  try {
    const r = await http.get('/api/playnite/games');
    const games: any[] = Array.isArray(r.data) ? r.data : [];
    gameOptions.value = games
      .filter((g) => !!g.installed)
      .map((g) => ({ label: g.name || g.id, value: g.id }))
      .sort((a, b) => a.label.localeCompare(b.label));
  } catch (_) {}
  gamesLoading.value = false;
}

function openInstallConfirm() {
  showInstallConfirm.value = true;
}

async function confirmInstall() {
  installing.value = true;
  showInstallConfirm.value = false;
  installing.value = true;
  installOk.value = false;
  installErr.value = false;
  installErrorMsg.value = '';
  try {
    const r = await http.post('/api/playnite/install', { restart: true }, { validateStatus: () => true });
    let ok = false;
    let body: any = null;
    try { body = r.data; } catch {}
    ok = r.status >= 200 && r.status < 300 && body && body.status === true;
    if (ok) {
      installOk.value = true;
      await refreshStatus();
    } else {
      installErr.value = true;
      installErrorMsg.value = (body && body.error) || r.statusText || 'Unknown error';
    }
  } catch (e: any) {
    installErr.value = true;
    installErrorMsg.value = e?.message || '';
  }
  installing.value = false;
}

onMounted(async () => {
  // ensure config is loaded so platform/keys available
  if (!cfg.value) await store.fetchConfig();
  await refreshStatus();
  // Preload lists so existing selections display with names immediately
  loadGames();
  loadCategories();
});

const statusKind = computed<'active' | 'waiting' | 'session' | 'uninstalled' | 'unknown'>(() => {
  // Prefer login/session requirement over unknown detection
  if (!status.user_session_active) return 'session';
  if (!status.extensions_dir) return 'unknown';
  if (!status.installed) return 'uninstalled';
  return status.active ? 'active' : 'waiting';
});
const statusType = computed<'success' | 'warning' | 'error' | 'default'>(() => {
  switch (statusKind.value) {
    case 'active': return 'success';
    case 'waiting': return 'warning';
    case 'session': return 'warning';
    case 'uninstalled': return 'error';
    case 'unknown': return 'default';
    default: return 'default';
  }
});
const statusText = computed<string>(() => {
  switch (statusKind.value) {
    case 'active': return t('playnite.status_connected');
    case 'waiting': return t('playnite.status_waiting');
    case 'session': return t('playnite.status_session_required');
    case 'uninstalled': return t('playnite.status_uninstalled');
    case 'unknown': return (t('playnite.status_unknown') as any) || t('playnite.status_not_running_unknown');
    default: return '';
  }
});

function cmpSemver(a?: string, b?: string): number {
  if (!a || !b) return 0;
  const na = String(a).replace(/^v/i, '').split('.').map((x) => parseInt(x, 10));
  const nb = String(b).replace(/^v/i, '').split('.').map((x) => parseInt(x, 10));
  const len = Math.max(na.length, nb.length);
  for (let i = 0; i < len; i++) {
    const va = Number.isFinite(na[i]) ? na[i] : 0;
    const vb = Number.isFinite(nb[i]) ? nb[i] : 0;
    if (va < vb) return -1;
    if (va > vb) return 1;
  }
  return 0;
}

const pluginOutdated = computed(() => {
  if (!status.installed) return false;
  if (!status.plugin_version || !status.plugin_latest) return false;
  return cmpSemver(status.plugin_version, status.plugin_latest) < 0;
});
</script>

<style scoped>
</style>
