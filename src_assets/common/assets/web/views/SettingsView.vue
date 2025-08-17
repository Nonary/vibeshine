<template>
  <main ref="mainEl" class="flex-1 px-0 md:px-2 xl:px-6 py-2 md:py-6 space-y-6 overflow-x-hidden">
    <header
      class="sticky top-0 z-20 -mx-0 md:-mx-2 xl:-mx-6 px-0 md:px-2 xl:px-6 py-3 bg-light/70 dark:bg-dark/60 backdrop-blur supports-[backdrop-filter]:bg-light/50 supports-[backdrop-filter]:dark:bg-dark/40 border-b border-dark/10 dark:border-light/10"
    >
      <div class="flex items-center justify-between gap-4 flex-wrap">
        <div class="min-w-0">
          <h2 class="text-sm font-semibold uppercase tracking-wider">Settings</h2>
          <p class="text-[11px] opacity-60">
            Configuration auto-saves; restart to apply runtime changes.
          </p>
        </div>

        <div class="relative flex-1 max-w-2xl min-w-[260px]">
          <input
            v-model="searchQuery"
            type="text"
            placeholder="Search settings... (Enter to jump)"
            class="w-full px-3 py-2 text-sm rounded-md bg-black/5 dark:bg-white/10 text-dark dark:text-light placeholder-black/40 dark:placeholder-light/50 focus:outline-none focus:ring-2 focus:ring-primary/40"
            @focus="searchOpen = searchQuery.length > 0"
            @blur="() => setTimeout(() => (searchOpen = false), 120)"
            @keydown.enter.prevent="jumpFirstResult"
          />
          <i
            class="fas fa-magnifying-glass absolute right-3 top-1/2 -translate-y-1/2 text-[12px] opacity-60"
          />
          <transition name="fade">
            <div
              v-if="searchOpen"
              class="absolute mt-2 w-full z-20 bg-white/95 dark:bg-surface/95 backdrop-blur rounded-md shadow-lg border border-black/5 dark:border-white/10 max-h-80 overflow-auto"
            >
              <div v-if="searchResults.length === 0" class="px-3 py-2 text-[12px] opacity-60">
                No results
              </div>
              <button
                v-for="(r, idx) in searchResults"
                :key="idx"
                class="w-full text-left px-3 py-2 hover:bg-black/5 dark:hover:bg-white/10 text-[13px] flex items-start gap-2"
                @click="goTo(r)"
              >
                <span class="shrink-0 mt-0.5">
                  <i class="fas fa-compass text-primary text-[11px]" />
                </span>
                <span class="min-w-0">
                  <span class="block font-medium truncate">{{ r.label }}</span>
                  <span class="block text-[11px] opacity-60 truncate">{{ r.path }}</span>
                  <span
                    v-if="r.desc"
                    class="block text-[11px] opacity-70 line-clamp-2 break-words"
                    >{{ r.desc }}</span
                  >
                  <span
                    v-if="r.options && r.options.length"
                    class="block text-[11px] opacity-60 mt-1 truncate"
                    >Options:
                    {{
                      r.options
                        .map((o) => o.text)
                        .filter(Boolean)
                        .slice(0, 3)
                        .join(', ')
                    }}<span v-if="r.options.length > 3">…</span></span
                  >
                </span>
              </button>
            </div>
          </transition>
        </div>

        <div v-if="showSave" class="flex gap-2">
          <button class="btn" :disabled="saveState === 'saving'" @click="save">Save</button>
          <button v-if="saveState === 'saved' && !restarted" class="btn ghost" @click="apply">
            Apply
          </button>
        </div>
        <div v-else class="text-[11px] font-medium min-h-[1rem] flex items-center gap-2">
          <transition name="fade"><span v-if="saveState === 'saving'">Saving…</span></transition>
          <transition name="fade">
            <span v-if="saveState === 'saved'" class="text-green-600 dark:text-green-400"
              >Saved</span
            >
          </transition>
        </div>
      </div>
    </header>

    <div v-if="isReady" class="space-y-4">
      <section
        v-for="tab in tabs"
        :id="tab.id"
        :key="tab.id"
        :ref="(el) => sectionRefs.set(tab.id, el)"
        class="scroll-mt-24"
      >
        <button
          type="button"
          class="w-full flex items-center justify-between px-3 py-2 rounded-lg bg-black/5 dark:bg-white/10 hover:bg-black/10 dark:hover:bg-white/15 transition text-left"
          @click="toggle(tab.id)"
        >
          <span class="font-semibold">{{ tab.name }}</span>
          <i
            :class="[
              'fas text-xs transition-transform',
              isOpen(tab.id) ? 'fa-chevron-up' : 'fa-chevron-down',
            ]"
          />
        </button>
        <transition name="fade">
          <div
            v-show="isOpen(tab.id)"
            class="mt-2 bg-white/80 dark:bg-surface/70 backdrop-blur-sm border border-black/5 dark:border-white/10 rounded-xl shadow-sm p-6 space-y-6"
          >
            <component :is="tab.component" />
          </div>
        </transition>
      </section>
    </div>

    <div v-else class="text-xs opacity-60 space-y-2">
      <div v-if="isLoading">Loading...</div>
      <div v-else-if="isError" class="text-red-600 dark:text-red-400 space-y-2">
        <div>Failed to load configuration.</div>
        <button class="btn" :disabled="isLoading" @click="store.reloadConfig?.()">Retry</button>
      </div>
      <div v-else class="opacity-60">No configuration loaded.</div>
    </div>

    <div class="text-[11px]">
      <transition name="fade">
        <div
          v-if="saveState === 'saved' && !restarted && !autoSave"
          class="text-green-600 dark:text-green-400"
        >
          Saved. Click Apply to restart.
        </div>
      </transition>
      <transition name="fade">
        <div v-if="restarted" class="text-green-600 dark:text-green-400">Restart triggered.</div>
      </transition>
    </div>
  </main>

  <transition name="slide-fade">
    <div
      v-if="(dirty && !autoSave) || store.manualDirty === true"
      class="fixed bottom-4 right-6 z-30"
    >
      <div
        class="bg-white/90 dark:bg-surface/90 backdrop-blur rounded-lg shadow border border-black/5 dark:border-white/10 px-4 py-2 flex items-center gap-3"
      >
        <span class="text-[11px] font-medium">Unsaved changes</span>
        <button class="btn" :disabled="saveState === 'saving'" @click="save">Save</button>
      </div>
    </div>
  </transition>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, watch, markRaw } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import General from '@/configs/tabs/General.vue';
import Inputs from '@/configs/tabs/Inputs.vue';
import Network from '@/configs/tabs/Network.vue';
import Files from '@/configs/tabs/Files.vue';
import Advanced from '@/configs/tabs/Advanced.vue';
import AudioVideo from '@/configs/tabs/AudioVideo.vue';
import ContainerEncoders from '@/configs/tabs/ContainerEncoders.vue';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import { storeToRefs } from 'pinia';

const store = useConfigStore();
const { config } = storeToRefs(store);

// derive loading/error/ready from the store instead of local flags
const isLoading = computed(() => store.loading === true);
const isError = computed(() => store.error != null);
const isReady = computed(() => !!config.value && !isLoading.value && !isError.value);

const saveState = ref('idle'); // 'idle' | 'saving' | 'saved' | 'error'
const restarted = ref(false);
const dirty = ref(false);
const autoSave = ref(true);
const showSave = computed(() => store.manualDirty === true || !autoSave.value);

const mainEl = ref(null);
const searchQuery = ref('');
const searchOpen = ref(false);
const searchResults = ref([]);
const searchIndex = ref([]); // { sectionId, label, path, el }
const sectionRefs = new Map();

const tabs = [
  { id: 'general', name: 'General', component: markRaw(General) },
  { id: 'input', name: 'Input', component: markRaw(Inputs) },
  { id: 'av', name: 'Audio / Video', component: markRaw(AudioVideo) },
  { id: 'encoders', name: 'Encoders', component: markRaw(ContainerEncoders) },
  { id: 'network', name: 'Network', component: markRaw(Network) },
  { id: 'files', name: 'Files', component: markRaw(Files) },
  { id: 'advanced', name: 'Advanced', component: markRaw(Advanced) },
];

const openSections = ref(new Set(['general']));
const isOpen = (id) => openSections.value.has(id);
const toggle = (id) => {
  const s = new Set(openSections.value);
  s.has(id) ? s.delete(id) : s.add(id);
  openSections.value = s;
  // When expanding, (cheaply) rebuild index so new controls are searchable
  if (s.has(id)) queueBuildIndex();
};

onMounted(async () => {
  // Wait for authentication before calling APIs during mount
  const auth = useAuthStore();
  await auth.waitForAuthentication();
  await store.fetchConfig();
  if (config.value) queueBuildIndex();
});
// Rebuild index after auth becomes ready / authenticated to avoid needing a manual reload
const auth = useAuthStore();
// If auth store hasn't been initialized, ensure it does
onMounted(() => {
  if (auth && typeof auth.init === 'function') auth.init().catch(() => {});
});

// When auth becomes ready or authenticated, rebuild index (debounced a bit)
let authTimer = null;
watch(
  () => ({ ready: auth.ready, authed: auth.isAuthenticated }),
  () => {
    clearTimeout(authTimer);
    authTimer = setTimeout(() => queueBuildIndex(), 120);
  },
  { deep: true },
);
onUnmounted(() => {
  if (authTimer) clearTimeout(authTimer);
});

async function save() {
  // Guard autosave/background save when not authenticated
  if (!auth.isAuthenticated) return;
  if (!config.value) return;
  restarted.value = false;
  saveState.value = 'saving';
  const ok = await (store.save ? store.save() : Promise.resolve(false));
  if (ok) {
    saveState.value = 'saved';
    dirty.value = false;
  } else {
    saveState.value = 'error';
  }
}

async function apply() {
  await save();
  if (saveState.value !== 'saved') return;
  restarted.value = true;
  http.post(
    '/api/restart',
    {},
    { headers: { 'Content-Type': 'application/json' }, validateStatus: () => true },
  );
  setTimeout(() => {
    restarted.value = false;
    if (!autoSave.value) saveState.value = 'idle';
  }, 5000);
}

let t = null;
function debounceSave() {
  if (!autoSave.value) return;
  clearTimeout(t);
  t = setTimeout(save, 800);
}

// Mark dirty / autosave when version increments (user changed something)
watch(
  () => store.version,
  (v, oldV) => {
    if (!isReady.value || oldV === undefined) return; // ignore before ready
    dirty.value = true;
    if (store.savingState !== undefined) store.savingState = 'dirty';
    debounceSave();
  },
);

const route = useRoute();
const router = useRouter();

const goSection = (id) => {
  const dest = { path: '/settings', query: { sec: id } };
  route.path === '/settings' ? router.replace(dest) : router.push(dest);
};

function scrollToOpen(id) {
  if (!id) return;
  if (!isOpen(id)) toggle(id);
  const el = sectionRefs.get(id);
  if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' });
}
onMounted(() => {
  if (typeof route.query.sec === 'string') scrollToOpen(route.query.sec);
});
watch(
  () => route.query.sec,
  (id) => typeof id === 'string' && scrollToOpen(id),
);

function buildSearchIndex() {
  const root = mainEl.value;
  if (!root) return;
  const items = [];
  for (const sec of Array.from(root.querySelectorAll('section[id]'))) {
    const sectionId = sec.getAttribute('id');
    const sectionTitle = sec.querySelector('h3')?.textContent?.trim() || sectionId;
    for (const lbl of Array.from(sec.querySelectorAll('label'))) {
      const text = (lbl.textContent || '').trim();
      if (!text) continue;
      const forId = lbl.getAttribute('for');
      let target = null;
      if (forId) {
        try {
          target = sec.querySelector('#' + CSS.escape(forId));
        } catch {}
      }
      if (!target)
        target = lbl.closest('div')?.querySelector('input,select,textarea,.form-control');
      // Attempt to locate a descriptor/description element associated with this label.
      let descText = '';
      try {
        const isDescClass = (cls) =>
          !!cls &&
          (cls.includes('text-[11px]') || cls.includes('form-text') || cls.includes('text-xs'));
        // First search within the immediate container for a small description div.
        const container = lbl.parentElement;
        if (container) {
          const candidate = Array.from(container.querySelectorAll('div,p,small')).find(
            (d) => d !== lbl && isDescClass(d.className) && d.textContent.trim().length > 0,
          );
          if (candidate) descText = candidate.textContent.trim();
        }
        // Fallback: look at following siblings up to a few steps
        if (!descText) {
          let sib = lbl.nextElementSibling;
          let steps = 0;
          while (sib && steps < 6) {
            if (isDescClass(sib.className) && sib.textContent.trim()) {
              descText = sib.textContent.trim();
              break;
            }
            sib = sib.nextElementSibling;
            steps++;
          }
        }
      } catch {
        /* ignore */
      }
      // If the control is a select, gather option texts/values to allow matching
      let optionsList = [];
      let optionsText = '';
      try {
        if (target && target.tagName && target.tagName.toLowerCase() === 'select') {
          optionsList = Array.from(target.querySelectorAll('option')).map((o) => ({
            text: (o.textContent || '').trim(),
            value: o.value,
          }));
          optionsText = optionsList
            .map((o) => o.text)
            .filter(Boolean)
            .join(' | ');
        }
      } catch {
        optionsList = [];
        optionsText = '';
      }

      if (target)
        items.push({
          sectionId,
          label: text,
          path: `${sectionTitle} › ${text}`,
          el: target,
          desc: descText,
          options: optionsList,
          optionsText,
        });
    }
  }
  searchIndex.value = items;
}

let buildPending = false;
function queueBuildIndex() {
  if (buildPending) return;
  buildPending = true;
  requestAnimationFrame(() => {
    buildPending = false;
    buildSearchIndex();
  });
}

watch(searchQuery, (q) => {
  const v = (q || '').trim().toLowerCase();
  searchOpen.value = v.length > 0;
  if (!v) {
    searchResults.value = [];
    return;
  }

  // Score matches: label matches highest, path next, desc lowest. Boost exact startsWith.
  const scoreFor = (it) => {
    const lv = it.label.toLowerCase();
    const pv = it.path.toLowerCase();
    const dv = (it.desc || '').toLowerCase();
    const ov = (it.optionsText || '').toLowerCase();
    let score = 0;
    if (lv.includes(v)) score += 100 - lv.indexOf(v); // earlier in label is better
    if (lv.startsWith(v)) score += 50;
    if (pv.includes(v)) score += 40 - pv.indexOf(v) / 100; // smaller index better
    if (dv.includes(v)) score += 20 - dv.indexOf(v) / 1000;
    if (ov.includes(v)) score += 60 - ov.indexOf(v) / 10; // option text matches are strong
    // penalize very long descriptions/path to prefer concise matches
    score -= (pv.length + dv.length + ov.length) / 1000;
    return score;
  };

  searchResults.value = searchIndex.value
    .map((it) => ({ it, s: scoreFor(it) }))
    .filter((x) => x.s > 0)
    .sort((a, b) => b.s - a.s)
    .slice(0, 15)
    .map((x) => x.it);
});
function jumpFirstResult() {
  if (searchResults.value.length) goTo(searchResults.value[0]);
}
function goTo(item) {
  searchOpen.value = false;
  if (item?.sectionId) goSection(item.sectionId);
  setTimeout(() => {
    try {
      item.el?.scrollIntoView({ behavior: 'smooth', block: 'center' });
      flash(item.el);
    } catch {}
  }, 250);
}
function flash(el) {
  el?.classList.add('flash-highlight');
  setTimeout(() => el?.classList.remove('flash-highlight'), 1200);
}
</script>

<style scoped>
.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.25s;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}

.slide-fade-enter-active,
.slide-fade-leave-active {
  transition: all 0.25s;
}

.slide-fade-enter-from,
.slide-fade-leave-to {
  opacity: 0;
  transform: translateY(6px);
}

.flash-highlight {
  box-shadow: 0 0 0 3px rgba(253, 184, 19, 0.35);
  outline: 2px solid rgba(253, 184, 19, 0.35);
  border-radius: 6px;
  transition: box-shadow 0.2s;
}

.no-scrollbar::-webkit-scrollbar {
  display: none;
}

.no-scrollbar {
  -ms-overflow-style: none;
  scrollbar-width: none;
}
</style>
