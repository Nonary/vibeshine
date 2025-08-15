<template>
  <div v-if="open" class="fixed inset-0 z-50 flex items-start justify-center p-6 overflow-y-auto">
    <div class="fixed inset-0 bg-black/50 backdrop-blur-sm" @click="close" />
    <div
      class="relative w-full max-w-3xl mx-auto bg-white dark:bg-surface rounded-xl shadow-xl flex flex-col border border-black/10 dark:border-white/10">
      <div class="flex items-center justify-between px-6 py-4 border-b border-black/5 dark:border-white/10">
        <h2 class="font-semibold text-sm">
          {{ form.index === -1 ? 'Add Application' : 'Edit Application' }}
        </h2>
        <button class="text-black/50 dark:text-white/50 hover:text-red-600" @click="close">
          <i class="fas fa-times" />
        </button>
      </div>
      <div class="p-6 space-y-6 text-xs overflow-y-auto max-h-[70vh]">
        <div class="grid gap-4 sm:grid-cols-2">
          <div class="space-y-1 col-span-2">
            <label class="font-medium">Name</label>
            <input v-model="form.name" class="app-input" placeholder="Game or App Name" />
          </div>
          <div class="space-y-1 col-span-2 flex flex-col">
            <label class="font-medium">Image Path</label>
            <div class="flex items-start gap-3">
              <input v-model="form['image-path']" class="app-input font-mono flex-1" placeholder="/path/to/image.png" />
              <div
                class="w-24 h-32 bg-black/5 dark:bg-white/5 rounded overflow-hidden flex items-center justify-center">
                <img v-if="previewSrc" :src="previewSrc" class="max-w-full max-h-full object-contain" loading="lazy" />
                <div v-else class="text-2xl font-bold text-primary/40">
                  {{ form.name?.substring(0, 1) || '?' }}
                </div>
              </div>
            </div>
            <p class="text-[11px] text-black/60 dark:text-white/40 mt-1">
              Paste a URL or local image filename here. Save to persist.
            </p>
          </div>
          <div class="space-y-1 col-span-2">
            <label class="font-medium">Command (single)</label>
            <textarea v-model="cmdText" class="app-input font-mono h-20" placeholder="Executable command line" />
            <p class="text-black/60 dark:text-white/50">
              Enter the full command line to run (single string).
            </p>
          </div>
          <div class="space-y-1 col-span-2">
            <label class="font-medium">Working Directory</label>
            <input v-model="form['working-dir']" class="app-input font-mono" placeholder="C:/Games/App" />
          </div>
          <div class="space-y-1 col-span-2 grid grid-cols-2 gap-4">
            <label class="inline-flex items-center gap-2 text-[11px]"><input v-model="form['exclude-global-prep-cmd']"
                type="checkbox" class="app-checkbox" />
              Exclude Global Prep</label>
            <label class="inline-flex items-center gap-2 text-[11px]"><input v-model="form['auto-detach']"
                type="checkbox" class="app-checkbox" /> Auto
              Detach</label>
            <label class="inline-flex items-center gap-2 text-[11px]"><input v-model="form['wait-all']" type="checkbox"
                class="app-checkbox" /> Wait
              All</label>
            <label v-if="platform === 'windows'" class="inline-flex items-center gap-2 text-[11px]"><input
                v-model="form.elevated" type="checkbox" class="app-checkbox" />
              Elevated</label>
          </div>
          <div class="space-y-1 col-span-2 sm:col-span-1">
            <label class="font-medium">Exit Timeout (s)</label>
            <input v-model.number="form['exit-timeout']" type="number" min="0" class="app-input w-32" />
          </div>
        </div>
        <div>
          <div class="flex items-center justify-between mb-2">
            <h3 class="font-semibold text-xs uppercase tracking-wider">Prep Commands</h3>
            <button class="mini-btn" @click="addPrep"><i class="fas fa-plus" /> Add</button>
          </div>
          <div v-if="form['prep-cmd'].length === 0" class="text-[11px] text-black/60 dark:text-white/40">
            None
          </div>
          <div v-else class="space-y-3">
            <div v-for="(p, i) in form['prep-cmd']" :key="i" class="grid gap-2 md:grid-cols-5 items-start">
              <input v-model="p.do" placeholder="do" class="app-input font-mono md:col-span-2" />
              <input v-model="p.undo" placeholder="undo" class="app-input font-mono md:col-span-2" />
              <div class="flex items-center gap-2 md:col-span-1">
                <label v-if="platform === 'windows'" class="inline-flex items-center gap-1 text-[11px]"><input
                    v-model="p.elevated" type="checkbox" class="app-checkbox" /> Elev</label>
                <button class="mini-btn danger" @click="form['prep-cmd'].splice(i, 1)">
                  <i class="fas fa-trash" />
                </button>
              </div>
            </div>
          </div>
        </div>
        <div>
          <div class="flex items-center justify-between mb-2">
            <h3 class="font-semibold text-xs uppercase tracking-wider">Detached Commands</h3>
            <button class="mini-btn" @click="form.detached.push('')">
              <i class="fas fa-plus" /> Add
            </button>
          </div>
          <div v-if="form.detached.length === 0" class="text-[11px] text-black/60 dark:text-white/40">
            None
          </div>
          <div v-else class="space-y-2">
            <div v-for="(d, i) in form.detached" :key="i" class="flex gap-2 items-start">
              <input v-model="form.detached[i]" class="app-input font-mono flex-1" />
              <button class="mini-btn danger" @click="form.detached.splice(i, 1)">
                <i class="fas fa-times" />
              </button>
            </div>
          </div>
        </div>
      </div>
      <div class="flex items-center justify-between px-6 py-4 border-t border-black/5 dark:border-white/10 gap-3">
        <div class="flex gap-2">
          <button class="main-btn" :disabled="saving.v" @click="save">
            <i class="fas fa-save" /> <span class="hidden sm:inline">Save</span>
          </button>
          <button class="ghost-btn" @click="close">Cancel</button>
        </div>
        <button v-if="form.index !== -1" class="danger-btn" :disabled="saving.v" @click="del">
          <i class="fas fa-trash" />
        </button>
      </div>
    </div>
  </div>
</template>
<script setup>
import { reactive, watch, computed } from 'vue';
import { http } from '@/http.js';
const props = defineProps({
  modelValue: Boolean,
  platform: String,
  app: Object,
  index: { type: Number, default: -1 },
});
const emit = defineEmits(['update:modelValue', 'saved', 'deleted']);
const open = computed(() => props.modelValue);
function fresh() {
  return {
    name: '',
    output: '',
    cmd: '',
    index: -1,
    'exclude-global-prep-cmd': false,
    elevated: false,
    'auto-detach': true,
    'wait-all': true,
    'exit-timeout': 5,
    'prep-cmd': [],
    detached: [],
    'image-path': '',
    'working-dir': '',
  };
}
const form = reactive(fresh());
// Normalize cmd to single string; if older data has array, join with spaces
watch(
  () => props.app,
  (val) => {
    if (!open.value) return;
    const copy = JSON.parse(JSON.stringify(val || {}));
    if (Array.isArray(copy.cmd)) copy.cmd = copy.cmd.join(' ');
    Object.assign(form, fresh(), copy);
    form.index = props.index ?? -1;
  },
  { immediate: true },
);
const cmdText = computed({
  get() {
    return form.cmd || '';
  },
  set(v) {
    form.cmd = v;
  },
});
watch(open, (o) => {
  if (o) {
    const copy = JSON.parse(JSON.stringify(props.app || {}));
    if (Array.isArray(copy.cmd)) copy.cmd = copy.cmd.join(' ');
    Object.assign(form, fresh(), copy);
    form.index = props.index ?? -1;
  }
});
function close() {
  emit('update:modelValue', false);
}
function addPrep() {
  form['prep-cmd'].push({
    do: '',
    undo: '',
    ...(props.platform === 'windows' ? { elevated: false } : {}),
  });
}
const saving = reactive({ v: false });

function simpleHash(str) {
  let h = 2166136261 >>> 0;
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i);
    h = Math.imul(h, 16777619) >>> 0;
  }
  return (h >>> 0).toString(36);
}
function coverSrc(app, index) {
  if (app?.uuid) {
    const cb = simpleHash(`${app.uuid}|${index ?? ''}`);
    return `./api/apps/${encodeURIComponent(app.uuid)}/cover?cb=${cb}`;
  }
  const path = (app?.['image-path'] || '').toString().trim();
  if (!path) return '';
  if (/^https?:\/\//i.test(path)) return path;
  if (!path.includes('/') && !path.includes('\\')) {
    return `./assets/${path}`;
  }
  const file = path.replace(/\\/g, '/').split('/').pop();
  if (file) {
    const cb = simpleHash(`${path}|${index ?? ''}`);
    const iParam = typeof index === 'number' ? `&i=${index}` : '';
    return `./covers/${file}?cb=${cb}${iParam}`;
  }
  return path;
}

const previewSrc = computed(() => {
  // prefer uuid cover if present, otherwise use image-path
  const a = form || {};
  if (a.uuid) return coverSrc(a, a.index);
  if (a['image-path']) return coverSrc(a, a.index);
  return '';
});
async function save() {
  saving.v = true;
  const payload = JSON.parse(JSON.stringify(form));
  try {
    await http.post('./api/apps', payload, {
      headers: { 'Content-Type': 'application/json' },
      validateStatus: () => true,
    });
    emit('saved');
    close();
  } finally {
    saving.v = false;
  }
}
async function del() {
  if (!confirm('Delete this application?')) return;
  saving.v = true;
  try {
    await http.delete(`./api/apps/${form.index}`, { validateStatus: () => true });
    emit('deleted');
    close();
  } finally {
    saving.v = false;
  }
}
</script>
<style scoped>
.app-input {
  width: 100%;
  border: 1px solid rgba(0, 0, 0, 0.12);
  background: rgba(255, 255, 255, 0.7);
  padding: 4px 8px;
  border-radius: 6px;
  font-size: 11px;
  line-height: 1.2;
}

.dark .app-input {
  background: rgba(10, 16, 40, 0.6);
  border-color: rgba(255, 255, 255, 0.15);
  color: #f5f9ff;
}

.app-input:focus {
  outline: 2px solid rgba(253, 184, 19, 0.5);
}

.dark .app-input:focus {
  outline: 2px solid rgba(77, 163, 255, 0.5);
}

.app-checkbox {
  width: 14px;
  height: 14px;
}

.main-btn,
.ghost-btn,
.danger-btn,
.mini-btn {
  font-size: 11px;
  font-weight: 500;
  line-height: 1;
  border-radius: 6px;
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 6px 10px;
  cursor: pointer;
}

.main-btn {
  background: rgba(253, 184, 19, 0.9);
  color: #212121;
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

.ghost-btn {
  background: transparent;
  color: #0d3b66;
}

.ghost-btn:hover {
  background: rgba(13, 59, 102, 0.1);
}

.dark .ghost-btn {
  color: #f5f9ff;
}

.dark .ghost-btn:hover {
  background: rgba(255, 255, 255, 0.1);
}

.danger-btn {
  background: #d32f2f;
  color: #fff;
  padding: 6px 10px;
}

.danger-btn:hover {
  background: #b71c1c;
}

.mini-btn {
  background: rgba(0, 0, 0, 0.65);
  color: #fff;
  padding: 4px 8px;
}

.mini-btn:hover {
  background: rgba(0, 0, 0, 0.85);
}

.mini-btn.danger {
  background: #d32f2f;
}

.mini-btn.danger:hover {
  background: #b71c1c;
}
</style>
