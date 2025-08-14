<script setup>
import { ref } from 'vue'
import NvidiaNvencEncoder from './encoders/NvidiaNvencEncoder.vue'
import IntelQuickSyncEncoder from './encoders/IntelQuickSyncEncoder.vue'
import AmdAmfEncoder from './encoders/AmdAmfEncoder.vue'
import VideotoolboxEncoder from './encoders/VideotoolboxEncoder.vue'
import SoftwareEncoder from './encoders/SoftwareEncoder.vue'
import VAAPIEncoder from './encoders/VAAPIEncoder.vue'

const props = defineProps({
  currentTab: { type: String, default: '' }
})

import { useConfigStore } from '../../stores/config.js'
const store = useConfigStore()
const config = store.config
// Fallback: if no currentTab provided, show all stacked (modern single page mode)
const showAll = () => !props.currentTab
</script>

<template>
  <!-- NVIDIA NVENC Encoder Tab -->
  <NvidiaNvencEncoder v-if="showAll() || currentTab === 'nv'" />

  <!-- Intel QuickSync Encoder Tab -->
  <IntelQuickSyncEncoder v-if="showAll() || currentTab === 'qsv'" />

  <!-- AMD AMF Encoder Tab -->
  <AmdAmfEncoder v-if="showAll() || currentTab === 'amd'" />

  <!-- VideoToolbox Encoder Tab -->
  <VideotoolboxEncoder v-if="showAll() || currentTab === 'vt'" />

  <!-- VAAPI Encoder Tab -->
  <VAAPIEncoder v-if="showAll() || currentTab === 'vaapi'" />

  <!-- Software Encoder Tab -->
  <SoftwareEncoder v-if="showAll() || currentTab === 'sw'" />
</template>

<style scoped>
</style>
