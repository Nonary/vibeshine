<template>
  <div class="space-y-6">
    <div class="flex items-center justify-between">
      <h2 class="text-sm font-semibold uppercase tracking-wider">Applications</h2>
      <div class="flex items-center gap-2">
        <button class="main-btn" @click="openAdd"><i class="fas fa-plus" /> Add</button>
      </div>
    </div>
    <div class="grid gap-5 sm:grid-cols-2 lg:grid-cols-3 2xl:grid-cols-5">
      <div
        v-for="(app, i) in apps"
        :key="appKey(app, i)"
        class="group relative rounded-xl overflow-hidden border border-black/10 dark:border-white/10 bg-white/60 dark:bg-lunar-surface/60 shadow hover:shadow-lg transition flex flex-col"
        role="button"
        tabindex="0"
        @click="openEdit(app, i)"
      >
        <div
          class="aspect-[3/4] w-full bg-black/10 dark:bg-white/5 relative flex items-center justify-center p-3"
        >
          <img
            v-if="hasCover(app)"
            :key="appKey(app, i)"
            :src="coverSrc(app, i)"
            class="max-w-full max-h-full object-contain rounded"
            loading="lazy"
            @error="onImgError($event)"
          />
          <div
            v-else
            class="flex items-center justify-center text-4xl font-bold text-solar-primary/40 dark:text-lunar-primary/40"
          >
            {{ app.name?.substring(0, 1) || '?' }}
          </div>
          <div
            class="absolute top-2 right-2 flex gap-2 opacity-0 group-hover:opacity-100 transition"
          >
            <button class="mini-btn" @click.stop="openEdit(app, i)">
              <i class="fas fa-cog" />
            </button>
          </div>
        </div>
        <div class="p-3 flex flex-col flex-1 min-h-[110px]">
          <h3 class="text-sm font-semibold truncate">
            {{ app.name }}
          </h3>
          <div class="mt-auto flex items-center justify-end text-[10px]">
            <span v-if="app['prep-cmd']?.length" class="opacity-60"
              >{{ app['prep-cmd'].length }} prep</span
            >
          </div>
        </div>
      </div>
      <button
        class="aspect-[3/4] rounded-xl border border-dashed border-black/20 dark:border-white/15 flex flex-col items-center justify-center text-black/40 dark:text-white/30 hover:text-solar-primary dark:hover:text-lunar-primary hover:border-solar-primary/50 dark:hover:border-lunar-primary/50 transition"
        @click="openAdd"
      >
        <i class="fas fa-plus text-3xl mb-2" />
        <span class="text-xs font-medium">Add Application</span>
      </button>
    </div>
    <AppEditModal
      v-model="showModal"
      :app="currentApp"
      :index="currentIndex"
      @saved="reload"
      @deleted="reload"
    />
    <!-- Playnite integration removed for now -->
  </div>
</template>
<script setup>
import { ref, onMounted } from 'vue';
import AppEditModal from '@/components/AppEditModal.vue';
import { useAppsStore } from '@/stores/apps.js';
const appsStore = useAppsStore();
const apps = ref([]);
const platform = ref('');
const showModal = ref(false);
const currentApp = ref(null);
const currentIndex = ref(-1);
async function reload() {
  await appsStore.loadApps(true);
  apps.value = appsStore.apps;
  // ensure platform is present via config store if needed
  if (!platform.value) {
    try {
      const { useConfigStore } = await import('@/stores/config.js');
      const store = useConfigStore();
      await store.fetchConfig();
      platform.value = store.config.value?.platform || '';
    } catch {}
  }
}
onMounted(reload);
function openAdd() {
  currentApp.value = null;
  currentIndex.value = -1;
  showModal.value = true;
}
function openEdit(app, i) {
  currentApp.value = app;
  currentIndex.value = i;
  showModal.value = true;
}
// Playnite integration removed
function appKey(app, index) {
  const p = app && app['image-path'] ? app['image-path'].toString() : '';
  const id = app?.uuid || '';
  return `${app?.name || 'app'}|${id}|${p}|${index}`;
}
function simpleHash(str) {
  // Tiny non-crypto hash to vary cache-busting per path; deterministic
  let h = 2166136261 >>> 0;
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i);
    h = Math.imul(h, 16777619) >>> 0;
  }
  return (h >>> 0).toString(36);
}
function hasCover(app) {
  // 1. The app has an uploaded cover addressed by UUID (image-path empty) OR
  // 2. image-path exists AND is a http(s) URL, asset reference, or file with an allowed image extension.
  try {
    const p = app?.['image-path'] ? app['image-path'].toString().trim() : '';
    const hasUuid = !!app?.uuid;
    if (!p) return hasUuid; // allow potential uploaded cover by UUID
    // Remote URL always attempt (server will reject unsupported hosts upstream only for uploads)
    if (/^https?:\/\//i.test(p)) return true;
    // Asset reference (no slashes) – assume valid
    if (!p.includes('/') && !p.includes('\\')) return true;
    // Extract extension
    const file = p.replace(/\\/g, '/').split('/').pop() || '';
    const ext = file.substring(file.lastIndexOf('.') + 1).toLowerCase();
    const allowed = ['png', 'jpg', 'jpeg', 'webp', 'bmp'];
    return allowed.includes(ext);
  } catch {
    return false;
  }
}
function coverSrc(app, index) {
  if (app?.uuid) {
    const cb = simpleHash(`${app.uuid}|${index ?? ''}`);
    return `./api/apps/${encodeURIComponent(app.uuid)}/cover?cb=${cb}`;
  }
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
    return `./covers/${file}?cb=${cb}${iParam}`;
  }
  return p;
}
function onImgError(e) {
  // Hide broken image and fall back to letter tile by removing src
  const img = e?.target;
  if (img) {
    img.style.display = 'none';
  }
}
</script>
<style scoped>
.main-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  background: rgba(253, 184, 19, 0.9);
  color: #212121;
  font-size: 11px;
  font-weight: 500;
  padding: 6px 12px;
  border-radius: 6px;
}

.main-btn:hover {
  background: #fdb813;
}

.dark .main-btn {
  background: rgba(77, 163, 255, 0.85);
  color: #050b1e;
}

.dark .main-btn:hover {
  background: #4da3ff;
}

.mini-btn {
  background: rgba(0, 0, 0, 0.65);
  color: #fff;
  font-size: 11px;
  padding: 4px 8px;
  border-radius: 6px;
}

.mini-btn:hover {
  background: rgba(0, 0, 0, 0.85);
}
</style>
