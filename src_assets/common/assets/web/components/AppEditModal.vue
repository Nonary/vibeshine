<template>
  <UiModal :open="open" :dismissible="true" :backdrop-close="true" size="2xl" @update:open="v => emit('update:modelValue', v)">
    <template #icon>
      <div class="h-14 w-14 rounded-full bg-gradient-to-br from-primary/20 to-primary/10 text-primary flex items-center justify-center shadow-inner">
        <i class="fas fa-window-restore text-xl" />
      </div>
    </template>
    <template #title>
      <div class="flex items-center gap-3">
        <span class="text-xl font-semibold">{{ form.index === -1 ? 'Add Application' : 'Edit Application' }}</span>
        <span v-if="form.name" class="text-xs opacity-60">{{ form.name }}</span>
      </div>
    </template>

    <form class="space-y-6 text-sm" @submit.prevent="save" @keydown.ctrl.enter.stop.prevent="save">
      <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
        <div class="space-y-1 md:col-span-2">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Name</label>
          <input v-model="form.name" class="ui-input" placeholder="Game or App Name" />
        </div>
        <div class="space-y-1 md:col-span-2">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Command</label>
          <textarea v-model="cmdText" class="ui-input font-mono h-20" placeholder="Executable command line" />
          <p class="text-[11px] opacity-60">Enter the full command line (single string).</p>
        </div>
        <div class="space-y-1 md:col-span-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Working Dir</label>
          <input v-model="form['working-dir']" class="ui-input font-mono" placeholder="C:/Games/App" />
        </div>
        <div class="space-y-1 md:col-span-1">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Exit Timeout</label>
          <div class="flex items-center gap-2">
            <input v-model.number="form['exit-timeout']" type="number" min="0" class="ui-input w-28" />
            <span class="text-xs opacity-60">seconds</span>
          </div>
        </div>
        <div class="space-y-1 md:col-span-2">
          <label class="text-xs font-semibold uppercase tracking-wide opacity-70">Image Path</label>
          <input v-model="form['image-path']" class="ui-input font-mono" placeholder="/path/to/image.png" />
          <p class="text-[11px] opacity-60">Optional; stored only and not fetched by Sunshine.</p>
        </div>
      </div>

      <div class="grid grid-cols-2 gap-3">
        <label class="flex items-center gap-2 text-xs">
          <input v-model="form['exclude-global-prep-cmd']" type="checkbox" class="ui-checkbox" />
          Exclude Global Prep
        </label>
        <label class="flex items-center gap-2 text-xs">
          <input v-model="form['auto-detach']" type="checkbox" class="ui-checkbox" />
          Auto Detach
        </label>
        <label class="flex items-center gap-2 text-xs">
          <input v-model="form['wait-all']" type="checkbox" class="ui-checkbox" />
          Wait All
        </label>
        <label v-if="platform === 'windows'" class="flex items-center gap-2 text-xs">
          <input v-model="form.elevated" type="checkbox" class="ui-checkbox" />
          Elevated
        </label>
      </div>

      <section class="space-y-3">
        <div class="flex items-center justify-between">
          <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">Prep Commands</h3>
          <UiButton size="sm" variant="primary" @click="addPrep">
            <i class="fas fa-plus" /> Add
          </UiButton>
        </div>
        <div v-if="form['prep-cmd'].length === 0" class="text-[12px] opacity-60">None</div>
        <div v-else class="space-y-2">
          <div v-for="(p, i) in form['prep-cmd']" :key="i" class="grid md:grid-cols-12 gap-2">
            <input v-model="p.do" placeholder="do" class="ui-input font-mono md:col-span-5" />
            <input v-model="p.undo" placeholder="undo" class="ui-input font-mono md:col-span-5" />
            <div class="flex items-center gap-2 md:col-span-2">
              <label v-if="platform === 'windows'" class="flex items-center gap-1 text-xs">
                <input v-model="p.elevated" type="checkbox" class="ui-checkbox" /> Elev
              </label>
              <UiButton size="sm" variant="danger" @click="form['prep-cmd'].splice(i, 1)">
                <i class="fas fa-trash" />
              </UiButton>
            </div>
          </div>
        </div>
      </section>

      <section class="space-y-3">
        <div class="flex items-center justify-between">
          <h3 class="text-xs font-semibold uppercase tracking-wider opacity-70">Detached Commands</h3>
          <UiButton size="sm" variant="primary" @click="form.detached.push('')">
            <i class="fas fa-plus" /> Add
          </UiButton>
        </div>
        <div v-if="form.detached.length === 0" class="text-[12px] opacity-60">None</div>
        <div v-else class="space-y-2">
          <div v-for="(d, i) in form.detached" :key="i" class="flex gap-2 items-start">
            <input v-model="form.detached[i]" class="ui-input font-mono flex-1" />
            <UiButton size="sm" variant="danger" @click="form.detached.splice(i, 1)">
              <i class="fas fa-times" />
            </UiButton>
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
        <UiButton tone="ghost" variant="neutral" @click="close">Cancel</UiButton>
        <UiButton v-if="form.index !== -1" variant="danger" :disabled="saving.v" @click="showDeleteConfirm = true">
          <i class="fas fa-trash" /> Delete
        </UiButton>
        <UiButton variant="primary" :loading="saving.v" :disabled="saving.v" @click="save">
          <i class="fas fa-save" /> Save
        </UiButton>
      </div>
    </template>

    <UiConfirmModal
      v-model:open="showDeleteConfirm"
      :title="$t('apps.confirm_delete_title_named', { name: form.name || '' })"
      :message="$t('apps.confirm_delete_message_named', { name: form.name || '' })"
      :confirm-text="$t('apps.delete')"
      :cancelText="$t('cancel')"
      confirm-icon="fas fa-trash"
      variant="danger"
      icon="fas fa-triangle-exclamation"
      @confirm="del"
      @cancel="showDeleteConfirm = false"
    />
  </UiModal>
</template>

<script setup>
import { reactive, watch, computed, ref } from 'vue';
import { http } from '@/http';
import UiModal from '@/components/UiModal.vue';
import UiButton from '@/components/UiButton.vue';
import UiConfirmModal from '@/components/UiConfirmModal.vue';
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
const showDeleteConfirm = ref(false);

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
    await http.delete(`./api/apps/${form.index}`, { validateStatus: () => true });
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
