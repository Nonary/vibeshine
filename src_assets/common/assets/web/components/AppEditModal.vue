<template>
  <n-modal :show="open" :mask-closable="true" @update:show="(v) => emit('update:modelValue', v)">
    <n-card :bordered="false" style="max-width: 56rem; width: 100%">
      <template #header>
        <div class="flex items-center gap-3">
          <div
            class="h-14 w-14 rounded-full bg-gradient-to-br from-primary/20 to-primary/10 text-primary flex items-center justify-center shadow-inner"
          >
            <i class="fas fa-window-restore text-xl" />
          </div>
          <div class="flex flex-col">
            <span class="text-xl font-semibold">{{
              form.index === -1 ? 'Add Application' : 'Edit Application'
            }}</span>
            <span v-if="form.name" class="text-xs opacity-60">{{ form.name }}</span>
          </div>
        </div>
      </template>

      <form
        class="space-y-6 text-sm"
        @submit.prevent="save"
        @keydown.ctrl.enter.stop.prevent="save"
      >
        <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
          <div class="space-y-1 md:col-span-2">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Name</label>
            <!-- When adding a new app on Windows with Playnite enabled, allow picking a Playnite game -->
            <template v-if="isNew && isWindows && playniteEnabled">
              <div class="flex items-center gap-2">
                <n-select
                  v-model:value="selectedPlayniteId"
                  :options="playniteOptions"
                  :loading="gamesLoading"
                  filterable
                  :disabled="lockPlaynite"
                  placeholder="Select a Playnite game…"
                  class="flex-1"
                  @focus="loadPlayniteGames"
                  @update:value="onPickPlaynite"
                />
                <n-button v-if="lockPlaynite" size="small" tertiary @click="unlockPlaynite">
                  Change
                </n-button>
              </div>
              <div v-if="!lockPlaynite" class="text-[11px] opacity-60">
                Pick from your Playnite library (installed only). Once selected, it locks in.
              </div>
            </template>
            <template v-else>
              <n-input v-model:value="form.name" placeholder="Game or App Name" />
            </template>
          </div>
          <div class="space-y-1 md:col-span-2">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Command</label>
            <n-input
              v-model:value="cmdText"
              type="textarea"
              :autosize="{ minRows: 4, maxRows: 8 }"
              placeholder="Executable command line"
            />
            <p class="text-[11px] opacity-60">Enter the full command line (single string).</p>
          </div>
          <div class="space-y-1 md:col-span-1">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
              >Working Dir</label
            >
            <n-input
              v-model:value="form['working-dir']"
              class="font-mono"
              placeholder="C:/Games/App"
            />
          </div>
          <div class="space-y-1 md:col-span-1">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
              >Exit Timeout</label
            >
            <div class="flex items-center gap-2">
              <n-input-number v-model:value="form['exit-timeout']" :min="0" class="w-28" />
              <span class="text-xs opacity-60">seconds</span>
            </div>
          </div>
          <div class="space-y-1 md:col-span-2">
            <label class="text-xs font-semibold uppercase tracking-wide opacity-70"
              >Image Path</label
            >
            <n-input
              v-model:value="form['image-path']"
              class="font-mono"
              placeholder="/path/to/image.png"
            />
            <p class="text-[11px] opacity-60">Optional; stored only and not fetched by Sunshine.</p>
          </div>
        </div>

        <div class="grid grid-cols-2 gap-3">
          <n-checkbox v-model:checked="form['exclude-global-prep-cmd']" size="small">
            Exclude Global Prep
          </n-checkbox>
          <n-checkbox v-model:checked="form['auto-detach']" size="small"> Auto Detach </n-checkbox>
          <n-checkbox v-model:checked="form['wait-all']" size="small">Wait All</n-checkbox>
          <n-checkbox v-if="platform === 'windows'" v-model:checked="form.elevated" size="small">
            Elevated
          </n-checkbox>
        </div>

        <section class="space-y-3">
          <div class="flex items-center justify-between">
            <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">Prep Commands</h3>
            <n-button size="small" type="primary" @click="addPrep">
              <i class="fas fa-plus" /> Add
            </n-button>
          </div>
          <div v-if="form['prep-cmd'].length === 0" class="text-[12px] opacity-60">None</div>
          <div v-else class="space-y-2">
            <div v-for="(p, i) in form['prep-cmd']" :key="i" class="grid md:grid-cols-12 gap-2">
              <n-input v-model:value="p.do" placeholder="do" class="font-mono md:col-span-5" />
              <n-input v-model:value="p.undo" placeholder="undo" class="font-mono md:col-span-5" />
              <div class="flex items-center gap-2 md:col-span-2">
                <n-checkbox v-if="platform === 'windows'" v-model:checked="p.elevated" size="small">
                  Elev
                </n-checkbox>
                <n-button size="small" type="error" @click="form['prep-cmd'].splice(i, 1)">
                  <i class="fas fa-trash" />
                </n-button>
              </div>
            </div>
          </div>
        </section>

        <section class="space-y-3">
          <div class="flex items-center justify-between">
            <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">
              Detached Commands
            </h3>
            <n-button size="small" type="primary" @click="form.detached.push('')">
              <i class="fas fa-plus" /> Add
            </n-button>
          </div>
          <div v-if="form.detached.length === 0" class="text-[12px] opacity-60">None</div>
          <div v-else class="space-y-2">
            <div v-for="(d, i) in form.detached" :key="i" class="flex gap-2 items-start">
              <n-input v-model:value="form.detached[i]" class="font-mono flex-1" />
              <n-button size="small" type="error" @click="form.detached.splice(i, 1)">
                <i class="fas fa-times" />
              </n-button>
            </div>
          </div>
        </section>
        <section class="sr-only">
          <!-- hidden submit to allow Enter to save within fields -->
          <button type="submit" tabindex="-1" aria-hidden="true"></button>
        </section>
      </form>

      <template #footer>
        <div class="flex items-center justify-end w-full gap-2">
          <n-button tertiary @click="close">Cancel</n-button>
          <n-button
            v-if="form.index !== -1"
            type="error"
            :disabled="saving.v"
            @click="showDeleteConfirm = true"
          >
            <i class="fas fa-trash" /> Delete
          </n-button>
          <n-button type="primary" :loading="saving.v" :disabled="saving.v" @click="save">
            <i class="fas fa-save" /> Save
          </n-button>
        </div>
      </template>

      <n-modal
        :show="showDeleteConfirm"
        :z-index="3300"
        :mask-style="{ backgroundColor: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(2px)' }"
        @update:show="(v) => (showDeleteConfirm = v)"
      >
        <n-card
          :title="
            isPlayniteAuto
              ? 'Remove and Exclude from Auto‑Sync?'
              : ($t('apps.confirm_delete_title_named', { name: form.name || '' }) as any)
          "
          :bordered="false"
          style="max-width: 32rem; width: 100%"
        >
          <div class="text-sm text-center space-y-2">
            <template v-if="isPlayniteAuto">
              <div>
                This application is managed by Playnite. Removing it will also add it to the
                Excluded Games list so it won’t be auto‑synced back.
              </div>
              <div class="opacity-80">
                You can bring it back later by manually adding it in Applications, or by removing
                the exclusion under Settings → Playnite.
              </div>
              <div class="opacity-70">Do you want to continue?</div>
            </template>
            <template v-else>
              {{ $t('apps.confirm_delete_message_named', { name: form.name || '' }) }}
            </template>
          </div>
          <template #footer>
            <div class="w-full flex items-center justify-center gap-3">
              <n-button tertiary @click="showDeleteConfirm = false">{{ $t('cancel') }}</n-button>
              <n-button type="error" @click="del">{{ $t('apps.delete') }}</n-button>
            </div>
          </template>
        </n-card>
      </n-modal>
    </n-card>
  </n-modal>
</template>

<script setup lang="ts">
import { reactive, watch, computed, ref } from 'vue';
import { http } from '@/http';
import { NModal, NCard, NButton, NInput, NInputNumber, NCheckbox, NSelect } from 'naive-ui';
import { storeToRefs } from 'pinia';
import { useConfigStore } from '@/stores/config';
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
const isPlaynite = computed(() => !!(form as any)['playnite-id']);
const isPlayniteAuto = computed(
  () => isPlaynite.value && (form as any)['playnite-managed'] !== 'manual',
);
watch(open, (o) => {
  if (o) {
    const copy = JSON.parse(JSON.stringify(props.app || {}));
    if (Array.isArray(copy.cmd)) copy.cmd = copy.cmd.join(' ');
    Object.assign(form, fresh(), copy);
    form.index = props.index ?? -1;
    // reset playnite picker state when opening
    selectedPlayniteId.value = '';
    lockPlaynite.value = false;
    // if editing an existing Playnite-managed app, keep name field simple
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
const showDeleteConfirm = ref(false);

// Platform + Playnite detection
const configStore = useConfigStore();
const { metadata } = storeToRefs(configStore);
const isWindows = computed(() => (metadata.value?.platform || '').toLowerCase() === 'windows');
const playniteEnabled = computed(() => {
  try {
    const v = (configStore.config as any)?.playnite_enabled;
    if (typeof v === 'boolean') return v;
    const s = String(v || '').toLowerCase();
    return s === 'enabled' || s === 'true' || s === 'on' || s === '1';
  } catch (_) {
    return false;
  }
});
const isNew = computed(() => form.index === -1);

// Playnite picker state
const gamesLoading = ref(false);
const playniteOptions = ref<{ label: string; value: string }[]>([]);
const selectedPlayniteId = ref('');
const lockPlaynite = ref(false);

async function loadPlayniteGames() {
  if (
    !isWindows.value ||
    !playniteEnabled.value ||
    gamesLoading.value ||
    playniteOptions.value.length
  )
    return;
  gamesLoading.value = true;
  try {
    const r = await http.get('/api/playnite/games');
    const games: any[] = Array.isArray(r.data) ? r.data : [];
    playniteOptions.value = games
      .filter((g) => !!g.installed)
      .map((g) => ({ label: g.name || g.id, value: g.id }))
      .sort((a, b) => a.label.localeCompare(b.label));
  } catch (_) {}
  gamesLoading.value = false;
}

function onPickPlaynite(id: string) {
  const opt = playniteOptions.value.find((o) => o.value === id);
  if (!opt) return;
  // Lock in selection and set fields
  form.name = opt.label;
  (form as any)['playnite-id'] = id;
  (form as any)['playnite-managed'] = 'manual';
  // clear command by default for Playnite managed entries
  if (!form.cmd) form.cmd = '';
  lockPlaynite.value = true;
}
function unlockPlaynite() {
  lockPlaynite.value = false;
}

// Cover preview logic removed; Sunshine no longer fetches or proxies images
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
  saving.v = true;
  try {
    // If Playnite auto-managed, add to exclusion list before removing
    const pid = (form as any)['playnite-id'];
    if (isPlayniteAuto.value && pid) {
      try {
        const cfg = await http.get('./api/config', { validateStatus: () => true });
        let raw = (cfg?.data && (cfg.data as any).playnite_exclude_games) || '';
        const set = new Set<string>();
        if (typeof raw === 'string')
          raw
            .split(',')
            .map((s) => s.trim())
            .filter(Boolean)
            .forEach((s) => set.add(s));
        set.add(String(pid));
        const next = Array.from(set).join(',');
        await http.post(
          './api/config',
          { playnite_exclude_games: next },
          { validateStatus: () => true },
        );
      } catch (_) {
        // best-effort; continue with deletion
      }
    }

    await http.delete(`./api/apps/${form.index}`, { validateStatus: () => true });
    // Best-effort force sync on Windows environments
    try {
      await http.post('./api/playnite/force_sync', {}, { validateStatus: () => true });
    } catch (_) {}
    emit('deleted');
    close();
  } finally {
    saving.v = false;
  }
}
</script>
<style scoped>
.ui-input {
  width: 100%;
  border: 1px solid rgba(0, 0, 0, 0.12);
  background: rgba(255, 255, 255, 0.75);
  padding: 8px 10px;
  border-radius: 8px;
  font-size: 13px;
  line-height: 1.2;
}
.dark .ui-input {
  background: rgba(13, 16, 28, 0.65);
  border-color: rgba(255, 255, 255, 0.14);
  color: #f5f9ff;
}
.ui-checkbox {
  width: 14px;
  height: 14px;
}
</style>
