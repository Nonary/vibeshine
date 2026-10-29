<template>
  <n-space vertical size="large" class="steam-big-picture w-full">
    <div class="flex items-start justify-between gap-3 flex-wrap">
      <div class="min-w-0">
        <h3 class="text-sm font-semibold">{{ $t('steam_big_picture.title') }}</h3>
        <p class="text-[12px] opacity-70">{{ $t('steam_big_picture.desc') }}</p>
      </div>
      <n-tag size="small" :type="badge.type" data-testid="steam-big-picture-status">
        {{ badge.label }}
      </n-tag>
    </div>

    <n-alert v-if="error" type="error" :show-icon="true">{{ error }}</n-alert>
    <n-alert v-if="detail" type="warning" :show-icon="true" data-testid="steam-big-picture-detail">
      {{ detail }}
    </n-alert>
    <n-alert v-if="notice" type="success" :show-icon="true">{{ notice }}</n-alert>

    <div class="flex items-start justify-between gap-4">
      <div class="min-w-0">
        <label for="steam_big_picture_controls" class="text-sm font-medium">
          {{ $t('steam_big_picture.controls') }}
        </label>
        <div class="text-[11px] opacity-70 leading-5">
          {{ $t('steam_big_picture.controls_desc') }}
        </div>
      </div>
      <n-switch
        id="steam_big_picture_controls"
        class="shrink-0"
        :value="status?.enabled === true"
        :loading="saving"
        :disabled="!status || saving || busy || restarting"
        @update:value="setControls"
      />
    </div>

    <div
      v-if="showDebugPort"
      class="flex items-start justify-between gap-4"
      data-testid="steam-debug-port"
    >
      <div class="min-w-0">
        <div class="text-sm font-medium">{{ $t('steam_big_picture.debug_port') }}</div>
        <div class="text-[11px] opacity-70 leading-5">{{ debugPortDescription }}</div>
      </div>
      <n-button
        v-if="needsRestart"
        size="small"
        type="primary"
        strong
        class="shrink-0"
        :disabled="busy || restarting"
        @click="confirmAction('restart')"
      >
        {{ $t('steam_big_picture.restart_steam') }}
      </n-button>
      <n-button
        v-else
        size="small"
        type="primary"
        strong
        class="shrink-0"
        :disabled="busy || restarting || !debugPort?.steam_found"
        @click="confirmAction('enable')"
      >
        {{ $t('steam_big_picture.turn_on_debug_port') }}
      </n-button>
    </div>

    <n-card
      v-if="status?.enabled"
      size="small"
      embedded
      :title="$t('steam_big_picture.compatibility')"
      data-testid="steam-compatibility"
    >
      <n-space vertical size="medium">
        <div class="flex items-start justify-between gap-4">
          <div class="text-[11px] opacity-70 leading-5 min-w-0">
            {{ $t('steam_big_picture.compatibility_desc') }}
          </div>
          <n-button size="small" class="shrink-0" :loading="checkBusy" @click="checkCompatibility">
            {{
              checkBusy
                ? $t('steam_big_picture.checking')
                : $t('steam_big_picture.check_compatibility')
            }}
          </n-button>
        </div>
        <div v-if="checkReport" role="status" data-testid="steam-compatibility-report">
          <p class="text-sm mb-2">{{ checkSummary }}</p>
          <ul class="space-y-1.5">
            <li v-for="row in checkRows" :key="row.name" class="flex items-center gap-2 flex-wrap">
              <n-tag size="small" :type="row.type">
                {{ $t(`steam_big_picture.check_results.${row.tone}`) }}
              </n-tag>
              <span class="text-sm">{{ row.label }}</span>
              <small v-if="row.tone !== 'success'" class="opacity-70">{{ row.result }}</small>
            </li>
          </ul>
        </div>
      </n-space>
    </n-card>
  </n-space>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { NAlert, NButton, NCard, NSpace, NSwitch, NTag, useDialog } from 'naive-ui';
import { useI18n } from 'vue-i18n';
import { http } from '@/http';
import { useConfigStore } from '@/stores/config';

interface BigPictureStatus extends ApiResult {
  enabled?: boolean;
  state?: string;
  detail?: string;
  debug_port?: {
    steam_found?: boolean;
    steam_running?: boolean;
    flag_file?: boolean;
    flag_file_created?: boolean;
  };
  steam_restart?: { state?: string; detail?: string };
}

interface CompatibilityReport {
  steam_build?: string;
  features?: Record<string, string>;
}

type Tone = 'success' | 'info' | 'neutral' | 'warning' | 'danger';
type DebugPortAction = 'enable' | 'restart' | 'disable';

interface DebugPortResult {
  status?: boolean;
  error?: string;
  error_code?: string;
  restart?: string;
}

const { t } = useI18n();
const dialog = useDialog();
const store = useConfigStore();

const status = ref<BigPictureStatus>();
const error = ref('');
const notice = ref('');
const saving = ref(false);
const busy = ref(false);
const checkBusy = ref(false);
const checkReport = ref<CompatibilityReport>();
// Polls the status while something is expected to change: a Steam restart, or the helper starting.
let statusTimer = 0;
let unmounted = false;

const tagTypes: Record<Tone, 'success' | 'info' | 'default' | 'warning' | 'error'> = {
  success: 'success',
  info: 'info',
  neutral: 'default',
  warning: 'warning',
  danger: 'error',
};

interface ApiResult {
  status?: boolean;
  error?: string;
}

async function request<T extends ApiResult>(
  method: 'get' | 'post' | 'patch',
  path: string,
  body?: unknown,
): Promise<T> {
  const res =
    method === 'get'
      ? await http.get<T>(path, { validateStatus: () => true })
      : await http[method]<T>(path, body ?? {}, { validateStatus: () => true });
  if (res.status >= 400) throw new Error(res.data?.error ?? '');
  return res.data;
}

function messageOf(cause: unknown, fallback: string): string {
  return cause instanceof Error && cause.message ? cause.message : fallback;
}

const restarting = computed(() => status.value?.steam_restart?.state === 'restarting');
const debugPort = computed(() => status.value?.debug_port ?? null);
// Steam has the flag file but was started before it existed, so its debug port is still closed.
const needsRestart = computed(
  () =>
    debugPort.value?.flag_file === true &&
    debugPort.value.steam_running === true &&
    status.value?.state === 'port_closed',
);
// Shown only when the controls are on but can't reach Steam yet: the flag file is missing, for
// example after turning them on while Steam wasn't found, or Steam needs a restart to read it.
const showDebugPort = computed(
  () =>
    status.value?.enabled === true &&
    debugPort.value !== null &&
    (!debugPort.value.flag_file || needsRestart.value),
);
const debugPortDescription = computed(() => {
  if (!debugPort.value?.steam_found) return t('steam_big_picture.debug_port_steam_missing');
  if (needsRestart.value) return t('steam_big_picture.debug_port_needs_restart');
  return t('steam_big_picture.debug_port_off_desc');
});

const badge = computed(() => {
  const value = status.value;
  const make = (key: string, tone: Tone) => ({
    label: t(`steam_big_picture.${key}`),
    type: tagTypes[tone],
  });
  if (!value) return make('status_unknown', 'neutral');
  if (!value.enabled) return make('status_off', 'neutral');
  if (restarting.value) return make('states.restarting', 'info');
  const tones: Record<string, Tone> = {
    injected: 'success',
    connecting: 'info',
    not_running: 'neutral',
    no_console_user: 'neutral',
    port_closed: 'warning',
    breaker_open: 'warning',
    port_foreign: 'danger',
    script_disabled: 'danger',
    helper_failed: 'danger',
    helper_stopped: 'danger',
    wrong_session: 'warning',
  };
  // The service starts the helper in the background, so for a moment after the controls are
  // turned on the status still says "off".
  const current = value.state === 'off' ? 'connecting' : (value.state ?? '');
  const tone = tones[current];
  if (!tone) return make('status_unknown', 'neutral');
  // The flag file is there but Steam was started before it existed.
  const state =
    current === 'port_closed' && value.debug_port?.flag_file ? 'needs_restart' : current;
  return make(`states.${state}`, tone);
});

// Why the controls aren't working, when Vibeshine or the script knows.
const detail = computed(() => (status.value?.enabled ? (status.value.detail?.trim() ?? '') : ''));

const featureLabels: Record<string, string> = {
  power_menu: 'steam_big_picture.features.power_menu',
  header_icon: 'steam_big_picture.features.header_icon',
  header_menu: 'steam_big_picture.features.header_menu',
  header_styles: 'steam_big_picture.features.header_styles',
  focusable: 'steam_big_picture.features.focusable',
};

// The script reports "off: <why>" and "fallback: <what instead>"; anything else means it works.
function featureTone(result: string): Tone {
  if (result.startsWith('off')) return 'danger';
  if (result.startsWith('fallback')) return 'warning';
  return 'success';
}

const checkRows = computed(() =>
  Object.entries(checkReport.value?.features ?? {}).map(([name, result]) => {
    const tone = featureTone(result);
    return {
      name,
      label: featureLabels[name] ? t(featureLabels[name]) : name.replaceAll('_', ' '),
      result,
      tone,
      type: tagTypes[tone],
    };
  }),
);

const checkSummary = computed(() => {
  const rows = checkRows.value;
  return t('steam_big_picture.check_summary', {
    working: rows.filter((row) => row.tone !== 'danger').length,
    total: rows.length,
    build: checkReport.value?.steam_build || t('steam_big_picture.check_unknown_build'),
  });
});

async function refreshStatus(): Promise<void> {
  try {
    status.value = await request<BigPictureStatus>('get', '/api/steam/big_picture/status');
  } catch (cause) {
    error.value = messageOf(cause, t('steam_big_picture.errors.status'));
  }
}

async function saveControls(value: boolean): Promise<void> {
  await request('patch', '/api/config', { steam_big_picture_controls: value });
  store.setSavedValue('steam_big_picture_controls', value);
  if (status.value) status.value.enabled = value;
}

function setControls(value: boolean): void {
  const port = debugPort.value;
  // Turning the controls on adds the flag file when Steam is there to take it, and turning them off
  // removes it only if Vibeshine added it. Both restart Steam, so they ask first. A file another
  // Steam mod added stays, and when Steam can't be reached the setting is saved on its own.
  const changesPort = value
    ? port?.steam_found === true && !port.flag_file
    : port?.flag_file_created === true;
  if (changesPort) {
    confirmAction(value ? 'enable' : 'disable');
    return;
  }
  void (async () => {
    saving.value = true;
    error.value = '';
    notice.value = '';
    try {
      await saveControls(value);
    } catch (cause) {
      error.value = messageOf(cause, t('steam_big_picture.errors.save'));
      return;
    } finally {
      saving.value = false;
    }
    notice.value = t('steam_big_picture.notices.saved');
    await refreshStatus();
    if (value) watchHelperStart();
  })();
}

function confirmAction(action: DebugPortAction, duringStream = false): void {
  // From the switch, turning the port on also turns the controls on.
  const titleKey = action === 'enable' && !status.value?.enabled ? 'controls_enable' : action;
  const options = duringStream
    ? {
        title: t('steam_big_picture.confirm.during_stream_title'),
        content: t('steam_big_picture.confirm.during_stream_desc'),
        positiveText: t('steam_big_picture.confirm.restart_anyway'),
      }
    : {
        title: t(`steam_big_picture.confirm.${titleKey}_title`),
        content: t(`steam_big_picture.confirm.${action}_desc`),
        positiveText: t(`steam_big_picture.confirm.${action}_button`),
      };
  dialog.warning({
    ...options,
    negativeText: t('_common.cancel'),
    onPositiveClick: () => {
      void runDebugPortAction(action, duringStream);
    },
  });
}

async function runDebugPortAction(action: DebugPortAction, duringStream: boolean): Promise<void> {
  busy.value = true;
  notice.value = '';
  error.value = '';
  const enabled = action !== 'disable';
  // From the switch the controls change with the port; the debug-port row only fixes the port.
  const changesControls = action !== 'restart' && status.value?.enabled !== enabled;
  let result: DebugPortResult = {};
  let savingSetting = false;
  try {
    let portError: unknown = null;
    try {
      result = await request<DebugPortResult>('post', '/api/steam/big_picture/debug_port', {
        enabled,
        restart_steam: true,
        allow_during_stream: duringStream,
      });
      if (result.status === false && result.error_code === 'stream_active' && !duringStream) {
        // Nothing changed yet: ask again, this time about the running stream.
        confirmAction(action, true);
        return;
      }
      if (result.status === false) {
        throw new Error(result.error || t('steam_big_picture.errors.debug_port'));
      }
    } catch (cause) {
      // The controls need the port to turn on, but turning them off doesn't wait for it.
      if (enabled || !changesControls) throw cause;
      result = {};
      portError = cause;
    }
    // Saved after the port, so a refused restart leaves the controls as they were.
    if (changesControls) {
      savingSetting = true;
      await saveControls(enabled);
      savingSetting = false;
    }
    if (portError) {
      error.value = messageOf(portError, t('steam_big_picture.errors.debug_port'));
    } else {
      notice.value =
        result.restart === 'started'
          ? t('steam_big_picture.notices.restarting')
          : t('steam_big_picture.notices.next_start');
    }
    await refreshStatus();
    if (result.restart === 'started') watchRestart();
    else if (enabled && changesControls) watchHelperStart();
  } catch (cause) {
    error.value = messageOf(
      cause,
      t(savingSetting ? 'steam_big_picture.errors.save' : 'steam_big_picture.errors.debug_port'),
    );
    // The port changed even though the setting didn't; keep following Steam's restart.
    if (result.restart === 'started') {
      await refreshStatus();
      watchRestart();
    }
  } finally {
    busy.value = false;
  }
}

// Follow the helper after the controls are turned on, until it gets past starting up and connecting
// to Steam, or give up after 30 s.
function watchHelperStart(started = Date.now()): void {
  window.clearTimeout(statusTimer);
  if (unmounted) return;
  statusTimer = window.setTimeout(() => void pollHelperStart(started), 1500);
}

async function pollHelperStart(started: number): Promise<void> {
  await refreshStatus();
  if (unmounted) return;
  const value = status.value;
  const starting =
    value?.enabled === true && (value.state === 'off' || value.state === 'connecting');
  if (starting && Date.now() - started < 30_000) watchHelperStart(started);
}

// Follow a Steam restart until Steam is back and the controls have reconnected, or give up after 90 s.
function watchRestart(started = Date.now()): void {
  window.clearTimeout(statusTimer);
  if (unmounted) return;
  statusTimer = window.setTimeout(() => void pollRestart(started), 1500);
}

async function pollRestart(started: number): Promise<void> {
  await refreshStatus();
  if (unmounted) return;
  const value = status.value;
  const restart = value?.steam_restart?.state;
  if (restart === 'failed') {
    notice.value = '';
    error.value = value?.steam_restart?.detail || t('steam_big_picture.errors.restart_failed');
    return;
  }
  const settled =
    restart !== 'restarting' &&
    (value?.state === 'injected' || !value?.enabled || !value?.debug_port?.flag_file);
  if (settled) {
    notice.value = t('steam_big_picture.notices.restarted');
  } else if (Date.now() - started < 90_000) {
    watchRestart(started);
  } else if (restart === 'restarting') {
    notice.value = '';
    error.value = t('steam_big_picture.errors.restart_slow');
  } else {
    // Steam is back but the controls haven't connected; the status tag says why.
    notice.value = t('steam_big_picture.notices.restarted');
  }
}

async function checkCompatibility(): Promise<void> {
  if (checkBusy.value) return;
  checkBusy.value = true;
  error.value = '';
  checkReport.value = undefined;
  try {
    const result = await request<{
      status?: boolean;
      error?: string;
      report?: CompatibilityReport;
    }>('post', '/api/steam/big_picture/check');
    if (result?.status === false || !result?.report) {
      throw new Error(result?.error || t('steam_big_picture.errors.check'));
    }
    checkReport.value = result.report;
  } catch (cause) {
    error.value = messageOf(cause, t('steam_big_picture.errors.check'));
  } finally {
    checkBusy.value = false;
  }
}

onMounted(() => {
  void refreshStatus();
});

onBeforeUnmount(() => {
  unmounted = true;
  window.clearTimeout(statusTimer);
});
</script>
