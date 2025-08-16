<template>
  <div class="mx-auto max-w-6xl px-4 py-6">
    <h1 class="mb-6 text-3xl font-semibold tracking-tight text-gray-900 dark:text-gray-100">
      {{ $t('troubleshooting.troubleshooting') }}
    </h1>

    <!-- Actions grid -->
    <div class="grid grid-cols-1 gap-6 lg:grid-cols-2">
      <!-- Force Close App -->
      <section
        class="rounded-2xl border border-gray-200 bg-white p-6 shadow-sm dark:border-gray-800 dark:bg-gray-900"
      >
        <header class="flex items-start justify-between gap-4">
          <div>
            <h2 id="close_apps" class="text-xl font-medium text-gray-900 dark:text-gray-100">
              {{ $t('troubleshooting.force_close') }}
            </h2>
            <p class="mt-2 text-sm text-gray-500 dark:text-gray-400">
              {{ $t('troubleshooting.force_close_desc') }}
            </p>
          </div>
          <button
            class="inline-flex items-center justify-center rounded-xl bg-amber-500 px-4 py-2 text-sm font-medium text-white shadow hover:bg-amber-600 disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="closeAppPressed"
            @click="closeApp"
          >
            {{ $t('troubleshooting.force_close') }}
          </button>
        </header>

        <transition name="fade">
          <p
            v-if="closeAppStatus === true"
            class="mt-4 rounded-lg border border-emerald-200 bg-emerald-50 px-4 py-2 text-sm text-emerald-800 dark:border-emerald-900/50 dark:bg-emerald-900/40 dark:text-emerald-100"
          >
            {{ $t('troubleshooting.force_close_success') }}
          </p>
        </transition>
        <transition name="fade">
          <p
            v-if="closeAppStatus === false"
            class="mt-4 rounded-lg border border-rose-200 bg-rose-50 px-4 py-2 text-sm text-rose-800 dark:border-rose-900/50 dark:bg-rose-900/40 dark:text-rose-100"
          >
            {{ $t('troubleshooting.force_close_error') }}
          </p>
        </transition>
      </section>

      <!-- Restart Sunshine -->
      <section
        class="rounded-2xl border border-gray-200 bg-white p-6 shadow-sm dark:border-gray-800 dark:bg-gray-900"
      >
        <header class="flex items-start justify-between gap-4">
          <div>
            <h2 id="restart" class="text-xl font-medium text-gray-900 dark:text-gray-100">
              {{ $t('troubleshooting.restart_sunshine') }}
            </h2>
            <p class="mt-2 text-sm text-gray-500 dark:text-gray-400">
              {{ $t('troubleshooting.restart_sunshine_desc') }}
            </p>
          </div>
          <button
            class="inline-flex items-center justify-center rounded-xl bg-amber-500 px-4 py-2 text-sm font-medium text-white shadow hover:bg-amber-600 disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="restartPressed"
            @click="restart"
          >
            {{ $t('troubleshooting.restart_sunshine') }}
          </button>
        </header>

        <transition name="fade">
          <p
            v-if="restartPressed === true"
            class="mt-4 rounded-lg border border-emerald-200 bg-emerald-50 px-4 py-2 text-sm text-emerald-800 dark:border-emerald-900/50 dark:bg-emerald-900/40 dark:text-emerald-100"
          >
            {{ $t('troubleshooting.restart_sunshine_success') }}
          </p>
        </transition>
      </section>

      <!-- Reset persistent display device settings (Windows only) -->
      <section
        v-if="platform === 'windows'"
        class="lg:col-span-2 rounded-2xl border border-gray-200 bg-white p-6 shadow-sm dark:border-gray-800 dark:bg-gray-900"
      >
        <header class="flex items-start justify-between gap-4">
          <div>
            <h2 id="dd_reset" class="text-xl font-medium text-gray-900 dark:text-gray-100">
              {{ $t('troubleshooting.dd_reset') }}
            </h2>
            <p class="mt-2 whitespace-pre-line text-sm text-gray-500 dark:text-gray-400">
              {{ $t('troubleshooting.dd_reset_desc') }}
            </p>
          </div>
          <button
            class="inline-flex items-center justify-center rounded-xl bg-amber-500 px-4 py-2 text-sm font-medium text-white shadow hover:bg-amber-600 disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="ddResetPressed"
            @click="ddResetPersistence"
          >
            {{ $t('troubleshooting.dd_reset') }}
          </button>
        </header>

        <transition name="fade">
          <p
            v-if="ddResetStatus === true"
            class="mt-4 rounded-lg border border-emerald-200 bg-emerald-50 px-4 py-2 text-sm text-emerald-800 dark:border-emerald-900/50 dark:bg-emerald-900/40 dark:text-emerald-100"
          >
            {{ $t('troubleshooting.dd_reset_success') }}
          </p>
        </transition>
        <transition name="fade">
          <p
            v-if="ddResetStatus === false"
            class="mt-4 rounded-lg border border-rose-200 bg-rose-50 px-4 py-2 text-sm text-rose-800 dark:border-rose-900/50 dark:bg-rose-900/40 dark:text-rose-100"
          >
            {{ $t('troubleshooting.dd_reset_error') }}
          </p>
        </transition>
      </section>
    </div>

    <!-- Logs -->
    <section
      class="mt-8 rounded-2xl border border-gray-200 bg-white shadow-sm dark:border-gray-800 dark:bg-gray-900"
    >
      <div class="flex flex-col gap-4 p-6 md:flex-row md:items-center md:justify-between">
        <div>
          <h2 id="logs" class="text-xl font-medium text-gray-900 dark:text-gray-100">
            {{ $t('troubleshooting.logs') }}
          </h2>
          <p class="mt-1 text-sm text-gray-500 dark:text-gray-400">
            {{ $t('troubleshooting.logs_desc') }}
          </p>
        </div>

        <div class="flex items-center gap-2">
          <input
            v-model="logFilter"
            type="text"
            :placeholder="$t('troubleshooting.logs_find')"
            class="w-80 rounded-xl border border-gray-300 bg-white px-3 py-2 text-sm text-gray-900 placeholder:text-gray-400 shadow-sm outline-none focus:border-amber-500 focus:ring-2 focus:ring-amber-500/20 dark:border-gray-700 dark:bg-gray-950 dark:text-gray-100"
            @input="handleFilterInput"
          />
          <button
            class="inline-flex items-center gap-2 rounded-xl border border-gray-300 bg-white px-3 py-2 text-sm font-medium text-gray-700 shadow-sm hover:bg-gray-50 dark:border-gray-700 dark:bg-gray-950 dark:text-gray-100 dark:hover:bg-gray-900"
            @click="toggleWrap"
          >
            <i :class="wrapLongLines ? 'fas fa-align-left' : 'fas fa-ellipsis-h'"></i>
            <span>{{ wrapLongLines ? 'Wrap' : 'No-wrap' }}</span>
          </button>
        </div>
      </div>

      <!-- Logs console -->
      <div class="relative">
        <!-- Sticky Copy Button -->
        <div class="pointer-events-none absolute right-4 top-4 z-20">
          <button
            :disabled="!actualLogs"
            class="pointer-events-auto inline-flex items-center gap-2 rounded-xl bg-gray-900/90 px-4 py-2 text-sm font-semibold text-white shadow-lg ring-1 ring-black/10 backdrop-blur hover:bg-gray-900 disabled:cursor-not-allowed disabled:opacity-50 dark:bg-white/90 dark:text-gray-900 dark:hover:bg-white"
            aria-label="Copy logs"
            @click="copyLogs"
          >
            <i class="fas fa-copy"></i>
            <span>{{ $t('_common.copy') }}</span>
          </button>
        </div>

        <!-- “New logs” Banner (appears at bottom of viewport inside console) -->
        <transition name="slide-up">
          <button
            v-if="newLogsAvailable"
            class="absolute bottom-4 left-1/2 z-20 -translate-x-1/2 rounded-full bg-amber-500 px-4 py-2 text-sm font-medium text-white shadow-lg hover:bg-amber-600"
            @click="jumpToLatest"
          >
            {{ newLogsLabel }}
            <span v-if="unseenLines > 0" class="ml-2 rounded bg-amber-600/20 px-2 py-0.5 text-xs">
              +{{ unseenLines }}
            </span>
            <i class="fas fa-arrow-down ml-2"></i>
          </button>
        </transition>

        <!-- Scroll container -->
        <div
          ref="logContainer"
          class="h-[520px] overflow-auto border-t border-gray-200 bg-gray-50 font-mono text-[13px] leading-5 text-gray-900 dark:border-gray-800 dark:bg-gray-950 dark:text-gray-100"
          @scroll.passive="onLogScroll"
        >
          <pre class="m-0 whitespace-pre p-4" :class="{ 'whitespace-pre-wrap': wrapLongLines }">{{
            actualLogs
          }}</pre>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onBeforeUnmount, nextTick } from 'vue';
import { useConfigStore } from '@/stores/config.js';
import { http } from '@/http.js';

const store = useConfigStore();
const platform = computed(
  () => store.platform || (navigator.userAgent.includes('Windows') ? 'windows' : ''),
);

// ==== State (client-unpairing removed) ====
const closeAppPressed = ref(false);
const closeAppStatus = ref(null);
const ddResetPressed = ref(false);
const ddResetStatus = ref(null);
const restartPressed = ref(false);

const logs = ref('Loading...');
const logFilter = ref('');
const wrapLongLines = ref(true);

const logContainer = ref(null);
const autoScrollEnabled = ref(true);
const newLogsAvailable = ref(false);
const unseenLines = ref(0);

let logInterval = null;
let lastLineCount = 0;

const filteredLines = computed(() => {
  if (!logFilter.value) return logs.value;
  const term = logFilter.value;
  return logs.value
    .split('\n')
    .filter((l) => l.includes(term))
    .join('\n');
});

const actualLogs = computed(() => filteredLines.value);

// Banner text (localizable)
const newLogsLabel = computed(() => {
  // Fall back to generic text if no localized key is suitable
  return 'New logs available — jump to latest';
});

// ==== Behavior ====
function handleFilterInput() {
  // Keep autoscroll if we’re at bottom when filtering; otherwise respect user scroll
  nextTick(() => {
    if (!logContainer.value) return;
    const atBottom = isNearBottom(logContainer.value);
    if (atBottom && autoScrollEnabled.value) {
      scrollToBottom();
    }
  });
}

function toggleWrap() {
  wrapLongLines.value = !wrapLongLines.value;
}

function onLogScroll() {
  if (!logContainer.value) return;
  const atBottom = isNearBottom(logContainer.value);
  if (atBottom) {
    autoScrollEnabled.value = true;
    newLogsAvailable.value = false;
    unseenLines.value = 0;
  } else {
    autoScrollEnabled.value = false;
  }
}

function isNearBottom(el) {
  const threshold = 24; // px tolerance
  return el.scrollTop + el.clientHeight >= el.scrollHeight - threshold;
}

function scrollToBottom() {
  if (!logContainer.value) return;
  logContainer.value.scrollTo({ top: logContainer.value.scrollHeight, behavior: 'smooth' });
}

async function refreshLogs() {
  try {
    const r = await http.get('./api/logs', {
      responseType: 'text',
      transformResponse: [(v) => v],
      validateStatus: () => true,
    });
    if (typeof r.data === 'string') {
      // Count new lines before replacing
      const prev = logs.value || '';
      const prevCount = prev ? prev.split('\n').length : 0;
      const nextText = r.data;
      const nextCount = nextText ? nextText.split('\n').length : 0;

      logs.value = nextText;

      // Track new lines if user is not at bottom
      if (!autoScrollEnabled.value) {
        const delta = Math.max(nextCount - prevCount, 0);
        if (delta > 0) {
          unseenLines.value += delta;
          newLogsAvailable.value = true;
        }
      }

      // Autoscroll if enabled
      await nextTick();
      if (autoScrollEnabled.value) {
        scrollToBottom();
        newLogsAvailable.value = false;
        unseenLines.value = 0;
      }

      lastLineCount = nextCount;
    }
  } catch {
    // ignore errors
  }
}

function jumpToLatest() {
  scrollToBottom();
  autoScrollEnabled.value = true;
  newLogsAvailable.value = false;
  unseenLines.value = 0;
}

async function copyLogs() {
  try {
    await navigator.clipboard.writeText(actualLogs.value || '');
  } catch {
    // no-op
  }
}

// ==== Actions ====
async function closeApp() {
  closeAppPressed.value = true;
  try {
    const r = await http.post('./api/apps/close', {}, { validateStatus: () => true });
    closeAppStatus.value = r.data?.status === true;
  } catch {
    closeAppStatus.value = false;
  } finally {
    closeAppPressed.value = false;
    setTimeout(() => (closeAppStatus.value = null), 5000);
  }
}

function restart() {
  restartPressed.value = true;
  setTimeout(() => (restartPressed.value = false), 5000);
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
    setTimeout(() => (ddResetStatus.value = null), 5000);
  }
}

// ==== Lifecycle ====
onMounted(() => {
  if (!store.config.value) {
    store.fetchConfig().catch(() => {});
  }

  // Start polling & ensure console starts at bottom
  nextTick(() => {
    if (logContainer.value) scrollToBottom();
  });

  logInterval = setInterval(refreshLogs, 5000);
  refreshLogs();
});

onBeforeUnmount(() => {
  if (logInterval) clearInterval(logInterval);
});
</script>

<style scoped>
/* Simple transitions using Tailwind-friendly classes */
.fade-enter-active,
.fade-leave-active {
  transition: opacity 200ms ease;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}

.slide-up-enter-active,
.slide-up-leave-active {
  transition:
    transform 200ms ease,
    opacity 200ms ease;
}

.slide-up-enter-from,
.slide-up-leave-to {
  transform: translateY(8px);
  opacity: 0;
}
</style>
