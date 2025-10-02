<template>
  <div class="max-w-4xl mx-auto px-6 py-8 space-y-6">
    <section class="space-y-2">
      <h2 class="text-sm font-semibold uppercase tracking-wider">
        {{ $t('apps.applications_title') || 'Applications' }}
      </h2>
      <p class="text-xs opacity-70">{{ $t('apps.applications_desc') }}</p>
      <p class="text-xs opacity-70">{{ $t('apps.applications_reorder_desc') }}</p>
      <pre class="text-xs opacity-70 whitespace-pre-wrap font-sans">
{{ $t('apps.applications_tips') }}
      </pre>
    </section>

    <div class="flex flex-wrap items-center justify-between gap-3">
      <n-space align="center" :size="12">
        <template v-if="isWindows">
          <n-button
            v-if="playniteEnabled"
            size="small"
            type="default"
            strong
            class="h-10 px-3 rounded-md"
            :loading="syncBusy"
            :disabled="syncBusy"
            @click="forceSync"
            aria-label="Force sync now"
          >
            <svg class="w-4 h-4 mr-2 inline-block" viewBox="0 0 24 24" fill="none" stroke="currentColor">
              <path
                stroke-linecap="round"
                stroke-linejoin="round"
                stroke-width="1.6"
                d="M21 12a9 9 0 11-3.2-6.6M21 3v6h-6"
              />
            </svg>
            {{ $t('playnite.force_sync') || 'Force Sync' }}
          </n-button>
          <n-button
            v-else
            size="small"
            type="default"
            strong
            class="h-10 px-3"
            @click="gotoPlaynite"
          >
            <svg class="w-4 h-4 mr-2 inline-block" viewBox="0 0 24 24" fill="none" stroke="currentColor">
              <path
                stroke-linecap="round"
                stroke-linejoin="round"
                stroke-width="1.6"
                d="M12 3v3m0 12v3m9-9h-3M6 12H3m13.95 5.657l-2.121-2.121M8.172 8.172 6.05 6.05m11.9 0-2.121 2.121M8.172 15.828 6.05 17.95"
              />
            </svg>
            {{ $t('playnite.setup_integration') || 'Setup Playnite' }}
          </n-button>
        </template>
      </n-space>

      <n-space align="center" :size="12">
        <n-button
          size="small"
          type="default"
          strong
          class="h-10 px-3"
          :disabled="!canAlphabetize || reorderBusy || !localApps.length"
          @click="alphabetize"
        >
          <i class="fas fa-sort-alpha-down mr-2" />{{ $t('apps.alphabetize') || 'Alphabetize' }}
        </n-button>
        <n-button
          v-if="listDirty"
          size="small"
          type="primary"
          strong
          class="h-10 px-4"
          :loading="reorderBusy"
          :disabled="reorderBusy"
          @click="saveOrder"
        >
          <i class="fas fa-save mr-2" />{{ $t('apps.save_order') || 'Save Order' }}
        </n-button>
        <n-button type="primary" size="small" strong class="h-10 px-4" @click="openAdd">
          <i class="fas fa-plus mr-2" />{{ $t('apps.add_new') || 'Add' }}
        </n-button>
      </n-space>
    </div>

    <div
      class="rounded-2xl border border-dark/10 dark:border-light/10 bg-light/80 dark:bg-surface/80 backdrop-blur divide-y divide-black/5 dark:divide-white/10"
    >
      <template v-if="localApps.length">
        <div
          v-for="(item, index) in localApps"
          :key="rowKey(item, index)"
          v-if="!item.isPlaceholder"
          class="flex items-center gap-4 px-5 py-4 transition-colors"
          :class="{
            'bg-primary/10 dark:bg-primary/15': isRunning(item),
            'ring-2 ring-primary/40 rounded-xl': item.dragover,
          }"
          :draggable="!showModal"
          @dragstart="onDragStart(index, $event)"
          @dragenter.prevent="onDragEnter(index)"
          @dragover.prevent="onDragOver"
          @dragleave.prevent="onDragLeave(index)"
          @drop.prevent="onDrop(index)"
          @dragend="onDragEnd"
        >
          <button
            type="button"
            class="shrink-0 w-6 h-10 flex items-center justify-center text-sm text-dark/40 dark:text-light/50 cursor-grab"
            aria-label="Reorder"
            @keydown.prevent
          >
            <i class="fas fa-grip-vertical" />
          </button>
          <div class="min-w-0 flex-1 cursor-pointer" @click="openEdit(item, item.originalIndex ?? index)">
            <div class="flex items-center gap-2 text-sm font-semibold truncate">
              <span class="truncate">{{ item.name || '(untitled)' }}</span>
              <n-tag
                v-if="item['playnite-id']"
                size="small"
                class="!px-2 !py-0.5 text-[10px] bg-slate-700 border-none text-slate-200"
              >
                Playnite
              </n-tag>
              <n-tag
                v-else
                size="small"
                class="!px-2 !py-0.5 text-[10px] bg-slate-700/70 border-none text-slate-200"
              >
                Custom
              </n-tag>
            </div>
            <div v-if="item['working-dir']" class="text-[11px] opacity-60 truncate mt-1">
              {{ item['working-dir'] }}
            </div>
          </div>
          <div class="flex items-center gap-2 shrink-0">
            <n-button
              size="small"
              type="primary"
              strong
              class="px-3"
              :loading="busyUuid === item.uuid"
              @click.stop="onLaunch(item)"
            >
              <template v-if="isRunning(item)">
                <i class="fas fa-stop mr-1" />{{ $t('_common.stop') || 'Stop' }}
              </template>
              <template v-else>
                <i class="fas fa-play mr-1" />{{ $t('apps.launch') || 'Launch' }}
              </template>
            </n-button>
            <n-button
              size="small"
              type="default"
              strong
              class="px-3"
              @click.stop="openEdit(item, item.originalIndex ?? index)"
            >
              <i class="fas fa-pen" />
            </n-button>
            <n-dropdown
              trigger="click"
              placement="bottom-end"
              :options="rowMenuOptions"
              @select="(key) => onRowMenuSelect(key, item)"
            >
              <n-button size="small" tertiary class="px-2">
                <i class="fas fa-ellipsis-h" />
              </n-button>
            </n-dropdown>
          </div>
        </div>
        <div
          v-else
          class="mx-5 my-4 h-12 border-2 border-dashed border-primary/40 rounded-xl"
          @dragenter.prevent="onDragEnter(index)"
          @dragover.prevent="onDragOver"
          @dragleave.prevent="onDragLeave(index)"
          @drop.prevent="onDrop(index)"
        ></div>
      </template>
      <div v-else class="py-12 text-center text-sm opacity-60">No applications configured.</div>
    </div>

    <AppEditModal
      v-model="showModal"
      :app="currentApp"
      :index="currentIndex"
      :key="
        modalKey +
        '|' +
        (currentIndex ?? -1) +
        '|' +
        (currentApp?.uuid || currentApp?.name || 'new')
      "
      @saved="reload"
      @deleted="reload"
    />
  </div>
</template>

<script setup lang="ts">
import { computed, defineAsyncComponent, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { useMessage, NButton, NSpace, NTag, NDropdown } from 'naive-ui';
import type { DropdownOption } from 'naive-ui';
import { useAppsStore, type App } from '@/stores/apps';
import { storeToRefs } from 'pinia';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';
import { useRouter } from 'vue-router';
import { useAuthStore } from '@/stores/auth';

const AppEditModal = defineAsyncComponent(() => import('@/components/AppEditModal.vue'));

interface LocalApp extends App {
  dragover?: boolean;
  isPlaceholder?: boolean;
  originalIndex?: number;
}

const { t } = useI18n();
const message = useMessage();

const appsStore = useAppsStore();
const { apps, currentAppUuid, hostName, hostUuid } = storeToRefs(appsStore);
const configStore = useConfigStore();
const auth = useAuthStore();
const router = useRouter();

const syncBusy = ref(false);
const playniteInstalled = ref(false);
const playniteStatusReady = ref(false);

const isWindows = computed(
  () => (configStore.metadata?.platform || '').toLowerCase() === 'windows',
);
const playniteEnabled = computed(() => playniteInstalled.value);

const showModal = ref(false);
const modalKey = ref(0);
const currentApp = ref<App | null>(null);
const currentIndex = ref<number | null>(-1);

const localApps = ref<LocalApp[]>([]);
const baselineOrder = ref<string[]>([]);
const draggingIndex = ref<number | null>(null);
const listDirty = ref(false);
const reorderBusy = ref(false);
const busyUuid = ref<string | null>(null);

const rowMenuOptions = computed<DropdownOption[]>(() => [
  {
    label: t('apps.export_launcher_file') || 'Export launcher file',
    key: 'export',
  },
]);

const canAlphabetize = computed(
  () => localApps.value.filter((item) => !item.isPlaceholder).length > 1,
);

watch(
  apps,
  (list) => {
    removePlaceholder();
    const next = Array.isArray(list)
      ? list.map((app, idx) => ({
          ...(app as App),
          dragover: false,
          isPlaceholder: false,
          originalIndex: idx,
        }))
      : [];
    localApps.value = next;
    baselineOrder.value = orderSignature(next);
    if (!showModal.value) {
      listDirty.value = false;
    }
  },
  { immediate: true },
);

const isLocalHost = computed(() => {
  if (typeof window === 'undefined') {
    return true;
  }
  const host = window.location.hostname;
  return host === 'localhost' || host === '127.0.0.1' || host === '[::1]';
});

function rowKey(item: LocalApp, index: number): string {
  return item.uuid || `${item.name ?? 'app'}-${item.originalIndex ?? index}`;
}

function isRunning(app: LocalApp): boolean {
  return !!app.uuid && app.uuid === currentAppUuid.value;
}

function ensurePlaceholder(): void {
  if (localApps.value.some((item) => item.isPlaceholder)) {
    return;
  }
  localApps.value.push({ name: '', isPlaceholder: true, originalIndex: -1 });
}

function removePlaceholder(): void {
  const idx = localApps.value.findIndex((item) => item.isPlaceholder);
  if (idx >= 0) {
    localApps.value.splice(idx, 1);
  }
}

function orderSignature(list: LocalApp[]): string[] {
  return list
    .filter((item) => !item.isPlaceholder)
    .map((item, idx) =>
      item.uuid ? `uuid:${item.uuid}` : `name:${item.name ?? ''}:${item.originalIndex ?? idx}`,
    );
}

function arraysEqual(a: string[], b: string[]): boolean {
  if (a.length !== b.length) {
    return false;
  }
  return a.every((value, idx) => value === b[idx]);
}

function alphabetize(): void {
  const filtered = localApps.value.filter((item) => !item.isPlaceholder);
  if (filtered.length < 2) {
    return;
  }
  const before = orderSignature(filtered);
  const sorted = [...filtered].sort((a, b) => {
    const left = (a.name || '').toLowerCase();
    const right = (b.name || '').toLowerCase();
    if (left === right) {
      return 0;
    }
    return left.localeCompare(right);
  });
  const after = orderSignature(sorted);
  if (arraysEqual(before, after)) {
    message.info(t('apps.already_ordered') || 'Already alphabetized.');
    return;
  }
  localApps.value = sorted.map((item) => ({ ...item, dragover: false, isPlaceholder: false }));
  listDirty.value = !arraysEqual(orderSignature(localApps.value), baselineOrder.value);
}

function onDragStart(index: number, event: DragEvent): void {
  if (showModal.value) {
    event.preventDefault();
    return;
  }
  draggingIndex.value = index;
  ensurePlaceholder();
  if (event.dataTransfer) {
    event.dataTransfer.effectAllowed = 'move';
    const uuid = localApps.value[index]?.uuid || '';
    try {
      event.dataTransfer.setData('text/plain', uuid);
    } catch {}
  }
}

function onDragEnter(index: number): void {
  const item = localApps.value[index];
  if (item) {
    item.dragover = true;
  }
}

function onDragLeave(index: number): void {
  const item = localApps.value[index];
  if (item) {
    item.dragover = false;
  }
}

function onDragOver(event: DragEvent): void {
  event.preventDefault();
  if (event.dataTransfer) {
    event.dataTransfer.dropEffect = 'move';
  }
}

function onDrop(index: number): void {
  if (draggingIndex.value === null) {
    return;
  }
  const before = orderSignature(localApps.value);
  const from = draggingIndex.value;
  const list = localApps.value;
  const targetItem = list[index];
  const placeholderIndex = list.findIndex((item) => item.isPlaceholder);
  const dropOnPlaceholder = !!targetItem?.isPlaceholder;

  const dragged = list[from];
  if (!dragged || dragged.isPlaceholder) {
    finalizeDrag();
    return;
  }

  list.splice(from, 1);

  let insertIndex = dropOnPlaceholder ? placeholderIndex : index;
  if (insertIndex < 0) {
    insertIndex = list.length;
  }
  if (from < insertIndex) {
    insertIndex -= 1;
  }

  const placeholderAfterRemoval = list.findIndex((item) => item.isPlaceholder);
  if (dropOnPlaceholder && placeholderAfterRemoval !== -1) {
    insertIndex = placeholderAfterRemoval;
  }

  if (insertIndex < 0) {
    insertIndex = 0;
  }
  if (insertIndex > list.length) {
    insertIndex = list.length;
  }

  list.splice(insertIndex, 0, dragged);

  finalizeDrag();

  const after = orderSignature(localApps.value);
  listDirty.value = !arraysEqual(after, baselineOrder.value);
}

function onDragEnd(): void {
  finalizeDrag();
}

function finalizeDrag(): void {
  removePlaceholder();
  draggingIndex.value = null;
  for (const item of localApps.value) {
    item.dragover = false;
  }
}

function openAdd(): void {
  modalKey.value += 1;
  currentApp.value = null;
  currentIndex.value = -1;
  showModal.value = true;
}

function openEdit(app: LocalApp, index: number): void {
  modalKey.value += 1;
  currentApp.value = app;
  currentIndex.value = index;
  showModal.value = true;
}

async function reload(): Promise<void> {
  await appsStore.loadApps(true);
}

async function saveOrder(): Promise<void> {
  const order = localApps.value
    .filter((item) => !item.isPlaceholder && item.uuid)
    .map((item) => String(item.uuid));

  if (!order.length) {
    message.warning(t('apps.reorder_failed') || 'Nothing to reorder.');
    return;
  }

  reorderBusy.value = true;
  try {
    await appsStore.reorder(order);
    await reload();
    listDirty.value = false;
    message.success(t('_common.saved') || 'Saved');
  } catch (err) {
    const details = extractError(err);
    message.error((t('apps.reorder_failed') || 'Failed to reorder apps:') + details);
  } finally {
    reorderBusy.value = false;
  }
}

function onRowMenuSelect(key: string | number, app: LocalApp): void {
  if (key === 'export') {
    exportLauncherFile(app);
  }
}

function exportLauncherFile(app: LocalApp): void {
  if (!app.uuid) {
    message.error('Unable to export launcher file for apps without a UUID.');
    return;
  }
  const uname = sanitizeFilename(app.name || 'app');
  const host = hostName.value || (typeof window !== 'undefined' ? window.location.hostname : 'Sunshine');
  const content = `# Artemis app entry\n# Exported by Apollo\n# https://github.com/ClassicOldSong/Apollo\n\n[host_uuid] ${hostUuid.value || ''}\n[host_name] ${host}\n[app_uuid] ${app.uuid}\n[app_name] ${app.name || ''}\n`;

  const blob = new Blob([content], { type: 'text/plain;charset=utf-8' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = `${uname}.apollo`;
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  URL.revokeObjectURL(url);
}

function sanitizeFilename(input: string): string {
  return input.replace(/[\\/:*?"<>|]/g, '_');
}

function openArtLink(app: LocalApp): void {
  if (typeof window === 'undefined') {
    return;
  }
  const url = `art://launch?host_uuid=${encodeURIComponent(hostUuid.value || '')}&host_name=${encodeURIComponent(
    hostName.value || window.location.hostname || 'Sunshine',
  )}&app_uuid=${encodeURIComponent(app.uuid || '')}&app_name=${encodeURIComponent(app.name || '')}`;
  const anchor = document.createElement('a');
  anchor.href = url;
  document.body.appendChild(anchor);
  anchor.click();
  document.body.removeChild(anchor);
}

async function onLaunch(app: LocalApp): Promise<void> {
  if (!app.uuid) {
    openEdit(app, app.originalIndex ?? -1);
    return;
  }

  if (isRunning(app)) {
    if (typeof window !== 'undefined' && !window.confirm(t('apps.close_warning'))) {
      return;
    }
    busyUuid.value = app.uuid;
    try {
      await appsStore.close();
      await reload();
    } catch (err) {
      const details = extractError(err);
      message.error((t('apps.close_failed') || 'Failed to close app:') + details);
    } finally {
      busyUuid.value = null;
    }
    return;
  }

  if (!isLocalHost.value && typeof window !== 'undefined') {
    if (window.confirm(t('apps.launch_local_client'))) {
      openArtLink(app);
      return;
    }
  }

  if (typeof window !== 'undefined' && !window.confirm(t('apps.launch_warning'))) {
    return;
  }

  busyUuid.value = app.uuid;
  try {
    await appsStore.launch(app.uuid);
    await reload();
  } catch (err) {
    const details = extractError(err);
    message.error((t('apps.launch_failed') || 'Failed to launch app:') + details);
  } finally {
    busyUuid.value = null;
  }
}

function extractError(err: unknown): string {
  const error = err as any;
  if (!error) {
    return '';
  }
  const responseError = error?.response?.data?.error;
  if (responseError) {
    return ` ${String(responseError)}`;
  }
  if (typeof error.message === 'string' && error.message.length) {
    return ` ${error.message}`;
  }
  return '';
}

async function forceSync(): Promise<void> {
  if (syncBusy.value) {
    return;
  }
  syncBusy.value = true;
  try {
    await http.post('/api/playnite/force_sync', {}, { validateStatus: () => true });
    await reload();
  } catch (err) {
    const details = extractError(err);
    message.error((t('playnite.force_sync_failed') || 'Playnite sync failed:') + details);
  } finally {
    syncBusy.value = false;
  }
}

function gotoPlaynite(): void {
  try {
    router.push({ path: '/settings', query: { sec: 'playnite' } });
  } catch {}
}

async function fetchPlayniteStatus(): Promise<void> {
  if (!auth.isAuthenticated) {
    return;
  }
  try {
    const r = await http.get('/api/playnite/status', { validateStatus: () => true });
    if (r.status === 200 && r.data && typeof r.data === 'object' && r.data !== null) {
      playniteInstalled.value = !!(r.data as any).installed;
    }
  } catch {}
  playniteStatusReady.value = true;
}

onMounted(async () => {
  try {
    await configStore.fetchConfig?.();
  } catch {}
  if (auth.isAuthenticated) {
    void fetchPlayniteStatus();
  } else {
    playniteStatusReady.value = false;
  }
  try {
    await appsStore.loadApps(true);
  } catch {}
});

auth.onLogin(() => {
  playniteStatusReady.value = false;
  void fetchPlayniteStatus();
});
</script>
