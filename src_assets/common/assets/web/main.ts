import { createApp, ref, watch, App as VueApp } from 'vue';
import { createPinia } from 'pinia';
import { initApp } from '@/init';
import { router } from '@/router';
import App from '@/App.vue';
import './styles/tailwind.css';
import { initHttpLayer } from '@/http';
import '@fortawesome/fontawesome-free/css/all.min.css';
import { useAuthStore } from '@/stores/auth';
import { useAppsStore } from '@/stores/apps';
import { useConfigStore } from '@/stores/config';

// Core application instance & stores
const app: VueApp<Element> = createApp(App);
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

  const auth = useAuthStore();
  const configStore = useConfigStore();
  const appsStore = useAppsStore();

  // Initialize auth status from server
  await auth.init();

  auth.waitForAuthentication().then(async () => {
    await configStore.fetchConfig(true);
    await appsStore.loadApps(true);
  });
});
