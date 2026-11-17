<script setup lang="ts">
import { onMounted, onBeforeUnmount, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { apiGet, apiPost } from '@/api/client';
const props = defineProps<{ selected: boolean }>();
const emit = defineEmits<{ availability: [boolean] }>();
const { t } = useI18n();
const status = ref({ available: false, ready: false, message: '' });
const busy = ref(false);
let timer: ReturnType<typeof setInterval> | undefined;
async function refresh() {
 try { status.value = await apiGet<typeof status.value>('/api/vibe-speakers/status'); emit('availability', status.value.available); }
 catch { status.value.message = t('config.vibe_status_failed'); }
}
async function setup() {
 if (busy.value) return;
 busy.value = true;
 try { const result = await apiPost<{status:boolean;error?:string}>('/api/vibe-speakers/setup', {});
  if (!result.status) status.value.message = result.error || t('config.vibe_status_failed');
 } catch { status.value.message = t('config.vibe_status_failed'); }
 finally { busy.value = false; }
}
watch(() => props.selected, (active, previous) => {
 if (active && !previous && status.value.available && !status.value.ready) void setup();
});
onMounted(() => { void refresh(); timer = setInterval(refresh, 3000); });
onBeforeUnmount(() => clearInterval(timer));
</script>
<template>
 <div v-if="selected || !status.available" class="vibe-speakers-status" role="status">
  <span :title="status.message">{{ status.ready ? t('config.vibe_ready') : status.message }}</span>
  <button v-if="selected && status.available && !status.ready" type="button" :disabled="busy" @click="setup">
   {{ t('config.vibe_setup') }}
  </button>
 </div>
</template>
