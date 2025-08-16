import { createApp, ref, watch } from 'vue';
import { createPinia } from 'pinia';
import { initApp } from '@/init';
import { router } from '@/router';
import App from '@/App.vue';
import './styles/tailwind.css';
import { initHttpLayer } from '@/http';
import '@fortawesome/fontawesome-free/css/all.min.css';

// Core application instance & stores
const app = createApp(App);
const pinia = createPinia();
app.use(pinia);
app.use(router);

// Expose platform ref early (updated after config load)
const platformRef = ref('');
app.provide('platform', platformRef);

// Central bootstrap: initialize i18n + auth status, then when authenticated load
// config & apps exactly once. Subsequent logouts (401) will re-trigger login modal
// and a later successful login will re-load fresh data.
initApp(app, async () => {
  await initHttpLayer();

  const [{ useAuthStore }, { useConfigStore }, { useAppsStore }] = await Promise.all([
    import('@/stores/auth'),
    import('@/stores/config'),
    import('@/stores/apps'),
  ]);

  const auth = useAuthStore();
  const configStore = useConfigStore();
  const appsStore = useAppsStore();

  let dataLoadGeneration = 0; // increment each auth cycle

  async function loadPostAuthData(gen) {
    try {
      await Promise.all([configStore.fetchConfig(true), appsStore.loadApps(true)]);
      // Only update platform if still same auth generation (user hasn't logged out/in again mid-load)
      if (gen === dataLoadGeneration) {
        platformRef.value = configStore.config?.value?.platform || '';
      }
    } catch (e) {
      console.error('post-auth data load failed', e);
    }
  }

  // Initialize auth status from server
  await auth.init();

  if (auth.isAuthenticated) {
    dataLoadGeneration += 1;
    loadPostAuthData(dataLoadGeneration);
  }

  // Watch for future successful logins
  watch(
    () => auth.isAuthenticated,
    (v) => {
      if (v) {
        dataLoadGeneration += 1;
        loadPostAuthData(dataLoadGeneration);
      }
    },
    { immediate: false },
  );
});
