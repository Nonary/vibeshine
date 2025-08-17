<template>
  <div class="space-y-6">
    <div class="flex items-center justify-between">
      <h2 class="text-sm font-semibold uppercase tracking-wider">Applications</h2>
      <div class="flex items-center gap-2">
        <button class="main-btn" @click="openAdd"><i class="fas fa-plus" /> Add</button>
      </div>
    </div>

    <!-- Redesigned list view -->
    <div class="rounded-2xl overflow-hidden border border-black/10 dark:border-white/10 bg-white/80 dark:bg-surface/80 backdrop-blur max-w-3xl mx-auto">
      <div v-if="apps && apps.length" class="divide-y divide-black/5 dark:divide-white/10">
        <button
          v-for="(app, i) in apps"
          :key="appKey(app, i)"
          type="button"
          class="w-full text-left focus:outline-none focus-visible:ring-2 focus-visible:ring-primary/40"
          @click="openEdit(app, i)"
          @keydown.enter.prevent="openEdit(app, i)"
          @keydown.space.prevent="openEdit(app, i)"
        >
          <div class="flex items-center justify-between px-4 py-3 hover:bg-black/5 dark:hover:bg-white/10">
            <div class="min-w-0 flex-1">
              <div class="text-sm font-semibold truncate">{{ app.name || '(untitled)' }}</div>
              <div class="mt-0.5 text-[11px] opacity-60 truncate" v-if="app['working-dir']">{{ app['working-dir'] }}</div>
            </div>
            <div class="shrink-0 text-dark/50 dark:text-light/60">
              <i class="fas fa-chevron-right" />
            </div>
          </div>
        </button>
      </div>
      <div v-else class="px-6 py-10 text-center text-sm opacity-60">No applications configured.</div>
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
/* Row chevron styling adapts via text color set inline */
</style>
