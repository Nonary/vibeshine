import { createApp, defineAsyncComponent, Component, AsyncComponentLoader } from 'vue';
import { createRouter, createWebHistory, RouteRecordRaw } from 'vue-router';
import { initApp } from './init';
import App from './App.vue';
import Home from './views/Home.vue';
import 'bootstrap/dist/css/bootstrap.min.css';
import { library } from '@fortawesome/fontawesome-svg-core';
import { FontAwesomeIcon } from '@fortawesome/vue-fontawesome';
import {
  faHome, faUnlock, faStream, faCog, faUserShield, faInfo, faCircleExclamation,
  faEdit, faTrash, faPlus, faPlay, faUndo, faShieldAlt, faCopy, faSun, faMoon,
  faCircleHalfStroke, faArrowLeft
} from '@fortawesome/free-solid-svg-icons';

library.add(
  faHome, faUnlock, faStream, faCog, faUserShield, faInfo, faCircleExclamation,
  faEdit, faTrash, faPlus, faPlay, faUndo, faShieldAlt, faCopy, faSun, faMoon,
  faCircleHalfStroke, faArrowLeft
);

interface LazyComponentOptions {
  loader: AsyncComponentLoader;
  delay: number;
  timeout: number;
  onError: (error: Error, retry: () => void, fail: () => void, attempts: number) => void;
}

const lazy = (loader: AsyncComponentLoader): Component =>
  defineAsyncComponent({
    loader,
    delay: 120,
    timeout: 20000,
    onError(_err: Error, retry: () => void, fail: () => void, attempts: number) {
      if (attempts <= 2) retry(); else fail();
    }
  } as LazyComponentOptions);

const routes: RouteRecordRaw[] = [
  { path: '/', component: Home },
  { path: '/apps', component: lazy(() => import('./views/Apps.vue')) },
  { path: '/clients', component: lazy(() => import('./views/Clients.vue')) },
  { path: '/config', component: lazy(() => import('./views/Config.vue')) },
  { path: '/pin', component: lazy(() => import('./views/Pin.vue')) },
  { path: '/password', component: lazy(() => import('./views/Password.vue')) },
  { path: '/troubleshooting', component: lazy(() => import('./views/Troubleshooting.vue')) },
  { path: '/welcome', component: lazy(() => import('./views/Welcome.vue')), meta: { standalone: true } },
  { path: '/:pathMatch(.*)*', name: 'NotFound', component: lazy(() => import('./views/NotFound.vue')) },
];

const router = createRouter({
  history: createWebHistory(),
  routes
});

const app = createApp(App);
app.use(router);
app.component('font-awesome-icon', FontAwesomeIcon);

initApp(app);