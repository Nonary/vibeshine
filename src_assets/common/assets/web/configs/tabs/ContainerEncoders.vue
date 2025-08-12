<script setup>
import { ref } from 'vue'
import NvidiaNvencEncoder from './encoders/NvidiaNvencEncoder.vue'
import IntelQuickSyncEncoder from './encoders/IntelQuickSyncEncoder.vue'
import AmdAmfEncoder from './encoders/AmdAmfEncoder.vue'
import VideotoolboxEncoder from './encoders/VideotoolboxEncoder.vue'
import SoftwareEncoder from './encoders/SoftwareEncoder.vue'
import VAAPIEncoder from './encoders/VAAPIEncoder.vue'

const props = defineProps({
    platform: String,
    config: Object,
    currentTab: { type: String, default: '' }
})

const config = ref(props.config)
// Fallback: if no currentTab provided, show all stacked (modern single page mode)
const showAll = () => !props.currentTab
</script>

<template>

  <!-- NVIDIA NVENC Encoder Tab -->
    <NvidiaNvencEncoder
            v-if="showAll() || currentTab === 'nv'"
      :platform="platform"
      :config="config"
  />

  <!-- Intel QuickSync Encoder Tab -->
    <IntelQuickSyncEncoder
            v-if="showAll() || currentTab === 'qsv'"
      :platform="platform"
      :config="config"
  />

  <!-- AMD AMF Encoder Tab -->
    <AmdAmfEncoder
            v-if="showAll() || currentTab === 'amd'"
      :platform="platform"
      :config="config"
  />

  <!-- VideoToolbox Encoder Tab -->
    <VideotoolboxEncoder
            v-if="showAll() || currentTab === 'vt'"
      :platform="platform"
      :config="config"
  />

  <!-- VAAPI Encoder Tab -->
    <VAAPIEncoder
            v-if="showAll() || currentTab === 'vaapi'"
      :platform="platform"
      :config="config"
  />

  <!-- Software Encoder Tab -->
    <SoftwareEncoder
            v-if="showAll() || currentTab === 'sw'"
      :platform="platform"
      :config="config"
  />

</template>

<style scoped>
</style>
