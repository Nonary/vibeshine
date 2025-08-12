<template>
  <div class="flex items-center gap-2 text-xs">
    <span :class="['h-2.5 w-2.5 rounded-full animate-pulse', streaming ? 'bg-green-500 shadow-[0_0_0_3px] shadow-green-500/30' : 'bg-gray-400']"></span>
    <span class="font-medium" v-text="streaming ? 'Live' : 'Idle'"/>
  </div>
</template>
<script setup>
import { ref, onMounted } from 'vue'
// Placeholder polling logic; backend endpoint not yet implemented
const streaming = ref(false)
onMounted(() => {
  // TODO replace with real endpoint
  setInterval(() => {
    streaming.value = streaming.value // keep as placeholder
  }, 10000)
})
</script>
<style scoped></style>
