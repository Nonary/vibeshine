import { createRouter, createWebHistory } from 'vue-router';
import { useAuthStore } from '@/stores/auth';
import DashboardView from '@/views/DashboardView.vue';
import ApplicationsView from '@/views/ApplicationsView.vue';
import SettingsView from '@/views/SettingsView.vue';
import TroubleshootingView from '@/views/TroubleshootingView.vue';
import ClientManagementView from '@/views/ClientManagementView.vue';
// Login view

const routes = [
  { path: '/', component: DashboardView },
  { path: '/applications', component: ApplicationsView },
  { path: '/settings', component: SettingsView },
  { path: '/logs', component: DashboardView },
  { path: '/troubleshooting', component: TroubleshootingView },
  { path: '/clients', component: ClientManagementView },
];

export const router = createRouter({
  // Use HTML5 history mode (no # in URLs)
  history: createWebHistory('/'),
  routes,
});

// Lightweight guard: if navigating to a protected route and not authenticated,
// open login modal (in-memory redirect) but allow navigation so URL stays.
router.beforeEach((to) => {
  if (typeof window === 'undefined') return true;
  try {
    const auth = useAuthStore();
    if (!auth.isAuthenticated) {
      auth.requireLogin(to.fullPath || to.path);
    }
  } catch (_) {}
  return true;
});
