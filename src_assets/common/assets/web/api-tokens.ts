import { createApp } from 'vue';
import { initApp } from '@/init';
// import Navbar from '@/Navbar.vue'; // Legacy - not needed
import ApiTokenManager from '@/ApiTokenManager.vue';

const app = createApp({
  components: { 
    // Navbar, // Legacy - not needed
    ApiTokenManager 
  },
  template: `
    <!-- <Navbar /> Legacy - navigation will be handled by backend -->
    <ApiTokenManager />
  `,
});

initApp(app);