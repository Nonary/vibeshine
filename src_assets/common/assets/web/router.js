import { createRouter, createWebHistory } from 'vue-router'
import { useAuthStore } from './stores/auth.js'
import DashboardView from './views/DashboardView.vue'
import ApplicationsView from './views/ApplicationsView.vue'
import SettingsView from './views/SettingsView.vue'
import TroubleshootingView from './views/TroubleshootingView.vue'
import ClientManagementView from './views/ClientManagementView.vue'
// Login view
import LoginView from './views/LoginView.vue'

const routes = [
  { path: '/', component: DashboardView },
  { path: '/login', component: LoginView, meta: { hideHeader: true } },
  { path: '/applications', component: ApplicationsView },
  { path: '/sessions', component: DashboardView },
  { path: '/settings', component: SettingsView },
  { path: '/logs', component: DashboardView },
  { path: '/troubleshooting', component: TroubleshootingView },
  { path: '/clients', component: ClientManagementView },
  { path: '/resources', component: DashboardView },
]

export const router = createRouter({
  // Use HTML5 history mode (no # in URLs)
  history: createWebHistory('/'),
  routes
})

// Simple auth guard: if not authenticated and not heading to login, redirect.
// We rely on server 401s + http.js for eventual consistency, but this prevents
// manual navigation to protected routes before first API call.
router.beforeEach((to) => {
  if (typeof window === 'undefined') return true
  // Allow direct access to login always
  if (to.path.startsWith('/login')) return true
  try {
    const auth = useAuthStore()
    if (!auth.isAuthenticated) {
      // Preserve intended path for post-login redirect via query param
      const target = encodeURIComponent(to.fullPath || to.path)
      return { path: '/login', query: { redirect: target } }
    }
  } catch (e) { /* swallow */ }
  return true
})
