import { createRouter, createWebHashHistory } from 'vue-router'
import DashboardView from './views/DashboardView.vue'
import ApplicationsView from './views/ApplicationsView.vue'
import SettingsView from './views/SettingsView.vue'
import TroubleshootingView from './views/TroubleshootingView.vue'

const routes = [
  { path: '/', component: DashboardView },
  { path: '/applications', component: ApplicationsView },
  { path: '/sessions', component: DashboardView },
  { path: '/settings', component: SettingsView },
  { path: '/logs', component: DashboardView },
  { path: '/troubleshooting', component: TroubleshootingView },
  { path: '/resources', component: DashboardView },
]

export const router = createRouter({
  history: createWebHashHistory(),
  routes
})
