<template>
  <div class="grid grid-cols-12 gap-6">
    <!-- Apps Grid -->
    <div class="col-span-12 xl:col-span-8 space-y-6">
      <div class="grid gap-6 sm:grid-cols-2 lg:grid-cols-3 2xl:grid-cols-4">
        <div
          v-for="(app, i) in apps"
          :key="i"
          class="group relative rounded-xl overflow-hidden border border-solar-dark/10 dark:border-lunar-light/10 bg-white/60 dark:bg-lunar-surface/60 shadow hover:shadow-lg transition flex flex-col"
        >
          <div class="aspect-[3/4] w-full relative bg-solar-dark/5 dark:bg-lunar-dark/30">
            <img
              v-if="hasCover(app)"
              :key="appKey(app, i)"
              :src="coverSrc(app, i)"
              class="absolute inset-0 w-full h-full object-cover"
              loading="lazy"
              @error="onImgError($event)"
            />
            <div
              v-else
              class="absolute inset-0 flex items-center justify-center text-4xl font-bold text-solar-primary/40 dark:text-lunar-primary/40"
            >
              {{ app.name.substring(0, 1) }}
            </div>
            <div
              class="absolute top-2 right-2 flex gap-1 opacity-0 group-hover:opacity-100 transition"
            >
              <button
                class="inline-flex items-center justify-center w-8 h-8 rounded-md bg-black/50 text-white hover:bg-black/70"
                @click="openEdit(app, i)"
              >
                <i class="fas fa-cog text-sm" />
              </button>
            </div>
          </div>
          <div class="p-3 flex flex-col flex-1">
            <h3 class="text-sm font-semibold truncate mb-1">
              {{ app.name }}
            </h3>
            <p class="text-[10px] uppercase tracking-wider opacity-60 line-clamp-2 mb-2">
              {{ displayCmd(app) }}
            </p>
            <div class="mt-auto flex items-center justify-between">
              <span
                class="px-2 py-0.5 rounded-full bg-green-500/15 text-green-600 dark:text-green-400 text-[10px]"
                >Configured</span
              >
              <ProfileSelector v-model="profileSelections[app.name]" />
            </div>
          </div>
        </div>
        <button
          class="aspect-[3/4] rounded-xl border border-dashed border-solar-dark/20 dark:border-lunar-light/10 flex flex-col items-center justify-center text-solar-dark/40 dark:text-lunar-light/30 hover:border-solar-primary/50 hover:text-solar-primary dark:hover:text-lunar-primary/80 transition"
          @click="openAdd"
        >
          <i class="fas fa-plus text-3xl mb-2" />
          <span class="text-xs font-medium">Add</span>
        </button>
      </div>
      <!-- Alerts / Warnings placeholder -->
      <div v-if="fatalLogs.length" class="space-y-3">
        <UiAlert variant="danger">
          <template #icon>
            <i class="fas fa-triangle-exclamation" />
          </template>
          <p class="font-medium">
            Startup errors detected ({{ fatalLogs.length }}). Review before streaming.
          </p>
        </UiAlert>
      </div>
    </div>

    <!-- Side widgets -->
    <div class="col-span-12 xl:col-span-4 space-y-6">
      <UiCard title="Streaming Health">
        <div class="grid grid-cols-3 gap-4 text-center text-xs">
          <div>
            <p class="text-[11px] uppercase tracking-wider mb-1 opacity-60">Resolution</p>
            <p class="font-semibold">--</p>
          </div>
          <div>
            <p class="text-[11px] uppercase tracking-wider mb-1 opacity-60">FPS</p>
            <p class="font-semibold">--</p>
          </div>
          <div>
            <p class="text-[11px] uppercase tracking-wider mb-1 opacity-60">Bitrate</p>
            <p class="font-semibold">--</p>
          </div>
        </div>
        <div class="mt-4 h-2 rounded-full bg-solar-dark/10 dark:bg-lunar-light/10 overflow-hidden">
          <div
            class="h-full w-1/3 bg-gradient-to-r from-green-500 via-yellow-400 to-red-500 animate-pulse"
          />
        </div>
      </UiCard>
      <UiCard title="Profiles">
        <div class="flex flex-wrap gap-2">
          <UiButton size="sm" tone="outline" variant="primary"> Gaming </UiButton>
          <UiButton size="sm" tone="outline" variant="primary"> Productivity </UiButton>
          <UiButton size="sm" tone="outline" variant="primary"> Low Latency </UiButton>
        </div>
      </UiCard>
      <UiCard title="Resources">
        <div class="text-xs space-y-2">
          <a
            href="https://app.lizardbyte.dev"
            target="_blank"
            class="block text-solar-primary dark:text-lunar-primary hover:underline"
            >Website</a
          >
          <a
            href="https://app.lizardbyte.dev/discord"
            target="_blank"
            class="block text-solar-primary dark:text-lunar-primary hover:underline"
            >Discord</a
          >
          <a
            href="https://github.com/orgs/LizardByte/discussions"
            target="_blank"
            class="block text-solar-primary dark:text-lunar-primary hover:underline"
            >GitHub Discussions</a
          >
        </div>
      </UiCard>
    </div>
  </div>
  <AppEditModal
    v-model="showModal"
    :app="currentApp"
    :index="currentIndex"
    @saved="reloadApps"
    @deleted="reloadApps"
  />
</template>
<script setup>
import { ref, onMounted, computed } from 'vue';
import UiButton from '@/components/UiButton.vue';
import UiAlert from '@/components/UiAlert.vue';
import UiCard from '@/components/UiCard.vue';
import ProfileSelector from '@/components/ProfileSelector.vue';
import AppEditModal from '@/components/AppEditModal.vue';
import { useAppsStore } from '@/stores/apps.js';
import { http } from '@/http.js';

const apps = ref([]);
const appsStore = useAppsStore();
const logs = ref('');
const profileSelections = ref({});
const platform = ref('');
// modal state reused from ApplicationsView
const showModal = ref(false);
const currentApp = ref(null);
const currentIndex = ref(-1);
async function load() {
  try {
    await appsStore.loadApps();
    apps.value = appsStore.apps;
  } catch (e) {
    console.error(e);
  }
  if (!platform.value) {
    try {
      const { useConfigStore } = await import('@/stores/config.js');
      const store = useConfigStore();
      await store.fetchConfig();
      platform.value = store.config.value?.platform || '';
    } catch (e) {}
  }
  try {
    const r = await http.get('./api/logs', {
      responseType: 'text',
      transformResponse: [(v) => v],
      validateStatus: () => true,
    });
    if (typeof r.data === 'string') logs.value = r.data;
  } catch (e) {}
}
onMounted(load);
function reloadApps() {
  appsStore.loadApps(true).then(() => {
    apps.value = appsStore.apps;
  });
}

const fatalLogs = computed(() => {
  if (!logs.value) return [];
  const regex = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g;
  const raw = logs.value.split(regex).splice(1);
  const out = [];
  for (let i = 0; i < raw.length; i += 2) {
    const line = raw[i + 1];
    if (line.startsWith('Fatal:')) out.push(line);
  }
  return out;
});

function openAdd() {
  currentApp.value = null;
  currentIndex.value = -1;
  showModal.value = true;
}
function openEdit(app, index) {
  currentApp.value = app;
  currentIndex.value = index;
  showModal.value = true;
}
function displayCmd(app) {
  if (Array.isArray(app.cmd)) return app.cmd.join(' ');
  return app.cmd || 'No command';
}
function appKey(app, index) {
  const p = app && app['image-path'] ? app['image-path'].toString() : '';
  const id = app?.uuid || '';
  return `${app?.name || 'app'}|${id}|${p}|${index}`;
}
function simpleHash(str) {
  let h = 2166136261 >>> 0;
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i);
    h = Math.imul(h, 16777619) >>> 0;
  }
  return (h >>> 0).toString(36);
}
function hasCover(app) {
  try {
    const p = app?.['image-path'] ? app['image-path'].toString().trim() : '';
    const hasUuid = !!app?.uuid;
    if (!p) return hasUuid;
    if (/^https?:\/\//i.test(p)) return true;
    if (!p.includes('/') && !p.includes('\\')) return true;
    const file = p.replace(/\\/g, '/').split('/').pop() || '';
    const ext = file.substring(file.lastIndexOf('.') + 1).toLowerCase();
    const allowed = ['png', 'jpg', 'jpeg', 'webp', 'bmp'];
    return allowed.includes(ext);
  } catch {
    return false;
  }
}
function coverSrc(app, index) {
  // Prefer UUID-based cover
  if (app?.uuid) {
    const cb = simpleHash(`${app.uuid}|${index ?? ''}`);
    // Use a relative path so the UI works when served from a subpath or behind a proxy
    return `./api/apps/${encodeURIComponent(app.uuid)}/cover?cb=${cb}`;
  }
  // Fallback to legacy image-path handling
  const path = app?.['image-path'];
  if (!path) return '';
  const p = path.toString().trim();
  if (/^https?:\/\//i.test(p)) return p;
  if (!p.includes('/') && !p.includes('\\')) {
    return `/assets/${p}`;
  }
  const file = p.replace(/\\/g, '/').split('/').pop();
  if (file) {
    const cb = simpleHash(`${p}|${index ?? ''}`);
    const iParam = typeof index === 'number' ? `&i=${index}` : '';
    // Use a relative path to match the server's routing and avoid absolute-root issues
    return `./covers/${file}?cb=${cb}${iParam}`;
  }
  return p;
}
function onImgError(e) {
  const img = e?.target;
  if (img) {
    img.style.display = 'none';
  }
}
</script>
<style scoped></style>
