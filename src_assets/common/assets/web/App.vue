<template>
  <div v-if="!route.meta.standalone">
    <Navbar />
    <div class="container mt-3">
      <router-view />
    </div>
  </div>
  <div v-else>
    <router-view />
  </div>
</template>
<script>
import { useRoute } from 'vue-router';
import Navbar from './Navbar.vue';
export default {
  components: { Navbar },
  setup() {
    const route = useRoute();
    return { route };
  },
};
</script>