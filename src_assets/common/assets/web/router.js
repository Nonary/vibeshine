import { createRouter, createWebHistory } from 'vue-router'
import DashboardView from './views/DashboardView.vue'
import ApplicationsView from './views/ApplicationsView.vue'
import SettingsView from './views/SettingsView.vue'
import TroubleshootingView from './views/TroubleshootingView.vue'
// Login view
import LoginView from './views/LoginView.vue'

const routes = [
  { path: '/', component: DashboardView },
  { path: '/login', component: LoginView },
  { path: '/applications', component: ApplicationsView },
  { path: '/sessions', component: DashboardView },
  { path: '/settings', component: SettingsView },
  { path: '/logs', component: DashboardView },
  { path: '/troubleshooting', component: TroubleshootingView },
  { path: '/resources', component: DashboardView },
]

export const router = createRouter({
  // Use HTML5 history mode (no # in URLs)
  history: createWebHistory('/'),
  routes
})
