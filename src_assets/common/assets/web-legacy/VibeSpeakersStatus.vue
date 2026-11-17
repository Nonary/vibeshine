<script setup lang="ts">
import { onMounted, onBeforeUnmount, ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { http } from '@/http';
defineProps<{ selected: boolean }>();
const emit = defineEmits<{ availability: [boolean] }>();
const { t } = useI18n();
const status = ref({ available: false, ready: false, message: '' });
let timer: ReturnType<typeof setInterval> | undefined;
async function refresh() {
 try { status.value = (await http.get('/api/vibe-speakers/status')).data; emit('availability', status.value.available); }
 catch { status.value.message = t('config.vibe_status_failed'); }
}
onMounted(() => { void refresh(); timer = setInterval(refresh, 3000); });
onBeforeUnmount(() => clearInterval(timer));
</script>
<template>
 <div v-if="selected || !status.available" class="vibe-speakers-status" role="status">
  <span :title="status.message">{{ status.ready ? t('config.vibe_ready') : status.message }}</span>
 </div>
</template>
