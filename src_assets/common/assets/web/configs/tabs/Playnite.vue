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
            v-model="config.playnite_enabled"
            id="playnite_enabled"
            :default-value="store.defaults.playnite_enabled"
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
        </div>
        <n-alert v-if="statusKind === 'session'" type="warning" :show-icon="true">
          {{ $t('playnite.session_required') }}
        </n-alert>
        <div class="flex items-center gap-2">
          <n-button type="primary" size="small" :loading="installing" @click="install">
            <i class="fas fa-plug" />
            <span class="ml-2">{{ status.installed ? ($t('playnite.reinstall_button') || 'Reinstall Plugin') : ($t('playnite.install_button') || 'Install Plugin') }}</span>
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
            v-model="config.playnite_auto_sync"
            id="playnite_auto_sync"
            :default-value="store.defaults.playnite_auto_sync"
            :localePrefix="'playnite'"
            :label="$t('playnite.auto_sync')"
            :desc="''"
          />
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.recent_games') }}</label>
          <n-input-number v-model:value="config.playnite_recent_games" :min="0" />
          <div class="text-[11px] opacity-70">{{ $t('playnite.recent_games_desc') }}</div>
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.recent_max_age_days') }}</label>
          <n-input-number v-model:value="config.playnite_recent_max_age_days" :min="0" />
          <div class="text-[11px] opacity-70">{{ $t('playnite.recent_max_age_days_desc') }}</div>
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.delete_after_days') }}</label>
          <n-input-number v-model:value="config.playnite_autosync_delete_after_days" :min="0" />
          <div class="text-[11px] opacity-70">{{ $t('playnite.delete_after_days_desc') }}</div>
        </div>
        <div class="md:col-span-2">
          <Checkbox
            v-model="config.playnite_autosync_require_replacement"
            id="playnite_autosync_require_replacement"
            :default-value="store.defaults.playnite_autosync_require_replacement"
            :localePrefix="'playnite'"
            :label="$t('playnite.require_replacement')"
            :desc="$t('playnite.require_replacement_desc')"
          />
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.focus_attempts') || 'Auto-focus attempts' }}</label>
          <n-input-number v-model:value="config.playnite_focus_attempts" :min="0" />
          <div class="text-[11px] opacity-70">{{ $t('playnite.focus_attempts_help') || 'Number of times to try to bring Playnite windows to the foreground when launching.' }}</div>
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.focus_timeout_secs') || 'Auto-focus timeout window (seconds)' }}</label>
          <n-input-number v-model:value="config.playnite_focus_timeout_secs" :min="0" />
          <div class="text-[11px] opacity-70">{{ $t('playnite.focus_timeout_secs_help') || 'How long auto-focus runs while re-applying focus (0 to disable).' }}</div>
        </div>
        <div class="md:col-span-1 flex items-end">
          <Checkbox
            v-model="config.playnite_focus_exit_on_first"
            id="playnite_focus_exit_on_first"
            :default-value="store.defaults.playnite_focus_exit_on_first"
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

      <div class="grid md:grid-cols-2 gap-4">
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.extensions_dir') }}</label>
          <n-input v-model:value="config.playnite_extensions_dir" />
        </div>
        <div>
          <label class="text-xs font-semibold">{{ $t('playnite.install_dir') || 'Playnite Install Directory' }}</label>
          <n-input v-model:value="config.playnite_install_dir" />
        </div>
      </div>
    </section>
  </div>
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
const platform = computed(() => (metadata.value?.platform || config.value?.platform || '').toLowerCase());
const { t } = useI18n();

const status = reactive<{
  installed: boolean;
  active: boolean;
  enabled?: boolean;
  user_session_active: boolean;
  session_required?: boolean;
  playnite_running?: boolean;
  extensions_dir: string;
}>({ installed: false, active: false, user_session_active: false, extensions_dir: '' });
const installing = ref(false);
const installOk = ref(false);
const installErr = ref(false);
const installErrorMsg = ref('');

const categoriesLoading = ref(false);
const gamesLoading = ref(false);
const categoryOptions = ref<{ label: string; value: string }[]>([]);
const gameOptions = ref<{ label: string; value: string }[]>([]);

const selectedCategories = computed<string[]>({
  get() {
    const raw = String(config.value?.playnite_sync_categories || '');
    // Support both comma and semicolon separated formats
    return raw
      .split(/[;,]/)
      .map((s) => s.trim())
      .filter(Boolean);
  },
  set(v: string[]) {
    // Server-side parse_list uses JSON array or comma-separated; use comma-separated
    store.updateOption('playnite_sync_categories', (v || []).join(','));
  },
});

const excludedIds = computed<string[]>({
  get() {
    const raw = (config.value?.playnite_exclude_games || '') as string;
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
      status.enabled = d.enabled !== undefined ? !!d.enabled : undefined;
      status.session_required = d.session_required;
      status.user_session_active = d.session_required === undefined ? true : !d.session_required;
      status.playnite_running = d.playnite_running;
      status.extensions_dir = d.extensions_dir || '';
    }
  } catch (_) {}
}

async function loadCategories() {
  if (platform.value !== 'windows') return;
  if (categoriesLoading.value || categoryOptions.value.length) return;
  categoriesLoading.value = true;
  try {
    // Prefer categories endpoint if available
    try {
      const rc = await http.get('/api/playnite/categories', { validateStatus: () => true });
      if (rc.status >= 200 && rc.status < 300 && Array.isArray(rc.data) && rc.data.length) {
        const cats = (rc.data as any[])
          .map((c) => String(c))
          .filter(Boolean)
          .sort((a, b) => a.localeCompare(b))
          .map((c) => ({ label: c, value: c }));
        categoryOptions.value = cats;
        return;
      }
    } catch {}
    // Fallback: derive from games list
    const rg = await http.get('/api/playnite/games');
    const games: any[] = Array.isArray(rg.data) ? rg.data : [];
    const set = new Set<string>();
    for (const g of games) for (const c of g?.categories || []) if (c && typeof c === 'string') set.add(c);
    categoryOptions.value = Array.from(set)
      .sort((a, b) => a.localeCompare(b))
      .map((c) => ({ label: c, value: c }));
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

async function install() {
  installing.value = true;
  installOk.value = false;
  installErr.value = false;
  installErrorMsg.value = '';
  try {
    const r = await http.post('/api/playnite/install', {}, { validateStatus: () => true });
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
  if (!config.value) await store.fetchConfig();
  await refreshStatus();
  // Preload lists so existing selections display with names immediately
  loadGames();
  loadCategories();
});

type StatusKind = 'disabled' | 'active' | 'session' | 'uninstalled' | 'not_running' | 'waiting';
const statusKind = computed<StatusKind>(() => {
  if (status.enabled === false) return 'disabled';
  if (status.active) return 'active';
  if (!status.user_session_active) return 'session';
  if (status.installed === false) return 'uninstalled';
  if (status.playnite_running === false) return 'not_running';
  if (status.installed === true && status.playnite_running === true && !status.active) return 'waiting';
  return 'disabled';
});
const statusType = computed<'success' | 'warning' | 'error' | 'default'>(() => {
  switch (statusKind.value) {
    case 'active': return 'success';
    case 'session': return 'warning';
    case 'waiting': return 'warning';
    case 'not_running': return 'error';
    case 'uninstalled': return 'error';
    case 'disabled':
    default:
      return 'default';
  }
});
const statusText = computed<string>(() => {
  switch (statusKind.value) {
    case 'active': return t('playnite.status_connected');
    case 'session': return t('playnite.status_desktop_required');
    case 'uninstalled': return t('playnite.status_uninstalled');
    case 'not_running': return t('playnite.status_not_running');
    case 'waiting': return t('playnite.status_waiting');
    case 'disabled':
    default:
      return t('playnite.status_disabled');
  }
});
</script>

<style scoped>
</style>
