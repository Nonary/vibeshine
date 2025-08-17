<template>
  <div class="space-y-6">
    <div class="flex items-center justify-between">
      <h2 class="text-sm font-semibold uppercase tracking-wider">Applications</h2>
      <div class="flex items-center gap-2">
        <button class="main-btn" @click="openAdd"><i class="fas fa-plus" /> Add</button>
      </div>
    </div>

    <!-- Simple list view -->
    <div class="rounded-xl overflow-hidden border border-black/10 dark:border-white/10 bg-white/60 dark:bg-surface/60">
      <div class="divide-y divide-black/5 dark:divide-white/10">
        <div
          v-for="(app, i) in apps"
          :key="appKey(app, i)"
          class="flex items-center justify-between px-4 py-3 hover:bg-black/5 dark:hover:bg-white/10 cursor-pointer"
          @click="openEdit(app, i)"
        >
          <div class="min-w-0 flex-1">
            <div class="text-sm font-medium truncate">{{ app.name || '(untitled)' }}</div>
            <div class="text-[11px] opacity-60 truncate">{{ app.cmd || '' }}</div>
          </div>
          <div class="flex items-center gap-3 shrink-0 text-[11px] opacity-70">
            <span v-if="app['prep-cmd']?.length">{{ app['prep-cmd'].length }} prep</span>
            <button class="mini-btn" @click.stop="openEdit(app, i)"><i class="fas fa-pen" /></button>
          </div>
        </div>
        <div v-if="!apps || apps.length === 0" class="px-4 py-8 text-center text-sm opacity-60">
          No applications configured.
        </div>
      </div>
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
import { useAppsStore } from '@/stores/apps';
import { storeToRefs } from 'pinia';
const appsStore = useAppsStore();
const { apps } = storeToRefs(appsStore);
const platform = ref('');
const showModal = ref(false);
const currentApp = ref(null);
const currentIndex = ref(-1);
async function reload() {
  await appsStore.loadApps(true);
}
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
  const id = app?.uuid || '';
  return `${app?.name || 'app'}|${id}|${index}`;
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
