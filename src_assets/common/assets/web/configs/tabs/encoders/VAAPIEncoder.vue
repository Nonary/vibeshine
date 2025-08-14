<script setup>
import { ref } from 'vue'
import Checkbox from "../../../Checkbox.vue";
import { useConfigStore } from '../../../stores/config.js'

const store = useConfigStore()
const config = store.config
</script>

<template>
  <div id="vaapi-encoder" class="config-page">
    <!-- Strict RC Buffer -->
    <Checkbox class="mb-3"
              id="vaapi_strict_rc_buffer"
              locale-prefix="config"
              v-model="config.vaapi_strict_rc_buffer"
              default="false"
    ></Checkbox>
  </div>
</template>

<style scoped>

</style>
