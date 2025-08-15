<template>
  <div class="flex items-center gap-2">
    <label v-if="label" class="text-[11px] font-medium uppercase tracking-wider opacity-60">{{
      label
      }}</label>
    <select v-model="model"
      class="text-xs bg-transparent border rounded-md px-2 py-1 focus:outline-none focus:ring-2 focus:ring-primary/40 border-dark/20">
      <option value="__default">Default</option>
      <option v-for="c in clients" :key="c.id" :value="c.id">
        {{ c.name }}
      </option>
    </select>
  </div>
</template>
<script setup>
import { ref, onMounted, watch } from 'vue';
import { useConfigStore } from '@/stores/config.js';
import { http } from '@/http.js';
const props = defineProps({
  modelValue: { type: String, default: '__default' },
  label: { type: String, default: '' },
});
const emit = defineEmits(['update:modelValue']);
const model = ref(props.modelValue);
const clients = ref([]);
const configStore = useConfigStore();

onMounted(async () => {
  try {
    // ensure config available (some environments expect platform to be present)
    if (!configStore.config.value)
      await configStore.fetchConfig().catch(() => {
        /* ignore */
      });
    const r = await http
      .get('./api/clients/list', { validateStatus: () => true })
      .then((r) => r.data || {});
    if (r && Array.isArray(r.named_certs)) {
      clients.value = r.named_certs.map((c) => ({
        id: c.uuid,
        name: c.name || c.uuid.substring(0, 8),
      }));
    }
  } catch (e) {
    console.warn('ProfileSelector: failed to load clients', e);
  }
});

watch(model, (v) => emit('update:modelValue', v));
watch(
  () => props.modelValue,
  (v) => {
    if (v !== model.value) model.value = v;
  },
);
</script>
<style scoped></style>
