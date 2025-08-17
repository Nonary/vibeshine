<template>
  <div class="min-h-screen flex flex-col bg-light dark:bg-dark text-dark dark:text-light">
    <header
      class="sticky top-0 z-30 h-14 flex items-center gap-4 px-4 border-b border-dark/10 dark:border-light/10 bg-light/70 dark:bg-dark/60 backdrop-blur supports-[backdrop-filter]:bg-light/40 supports-[backdrop-filter]:dark:bg-dark/40"
    >
      <div class="flex items-center gap-3 min-w-0">
        <img src="/images/logo-sunshine-45.png" alt="Sunshine" class="h-8 w-8" />
        <h1 class="text-base md:text-lg font-semibold tracking-tight truncate">
          {{ displayTitle }}
        </h1>
      </div>
      <nav class="hidden md:flex items-center gap-1 text-sm font-medium ml-2">
        <RouterLink to="/" :class="linkClass('/')">
          <i class="fas fa-gauge" /><span>Dashboard</span>
        </RouterLink>
        <RouterLink to="/applications" :class="linkClass('/applications')">
          <i class="fas fa-grid-2" /><span>Applications</span>
        </RouterLink>
        <RouterLink to="/clients" :class="linkClass('/clients')">
          <i class="fas fa-users-cog" /><span>{{ $t('clients.nav') }}</span>
        </RouterLink>
        <RouterLink to="/settings" :class="linkClass('/settings')">
          <i class="fas fa-sliders" /><span>Settings</span>
        </RouterLink>
        <RouterLink to="/troubleshooting" :class="linkClass('/troubleshooting')">
          <i class="fas fa-bug" /><span>{{ $t('navbar.troubleshoot') }}</span>
        </RouterLink>
        <a href="#" :class="linkClass('/logout')" @click.prevent="logout">
          <i class="fas fa-sign-out-alt" /><span>{{ $t('navbar.logout') }}</span>
        </a>
      </nav>
      <div class="ml-auto flex items-center gap-3 text-xs">
        <SavingStatus />
        <ThemeToggle />
      </div>
    </header>

    <!-- Content -->
    <main class="flex-1 overflow-auto p-4 md:p-6 space-y-6">
      <router-view />
    </main>
    <LoginModal />
    <transition name="fade-fast">
      <div v-if="loggedOut" class="fixed inset-0 z-[120] flex flex-col">
        <div
          class="absolute inset-0 bg-gradient-to-br from-white/70 via-white/60 to-white/70 dark:from-black/70 dark:via-black/60 dark:to-black/70 backdrop-blur-md"
        ></div>
        <div class="relative flex-1 flex flex-col items-center justify-center p-6 overflow-y-auto">
          <div class="w-full max-w-md mx-auto text-center space-y-6">
            <img
              src="/images/logo-sunshine-45.png"
              alt="Sunshine"
              class="h-24 w-24 opacity-80 mx-auto select-none"
            />
            <div class="space-y-2">
              <h2 class="text-2xl font-semibold tracking-tight">{{ $t('auth.logout_success') }}</h2>
              <p class="text-sm opacity-80 leading-relaxed">{{ $t('auth.logout_refresh_hint') }}</p>
            </div>
            <div class="flex items-center justify-center pt-2">
              <button
                type="button"
                class="inline-flex items-center justify-center gap-2 px-5 py-2.5 rounded-lg text-sm font-semibold tracking-wide bg-primary/80 text-onPrimary shadow hover:brightness-110 focus:outline-none focus-visible:ring-2 focus-visible:ring-offset-2 focus-visible:ring-primary/60 transition disabled:opacity-50"
                @click="refreshPage"
              >
                {{ $t('auth.logout_refresh_button') }}
                <i class="fas fa-rotate" />
              </button>
            </div>
            <p class="mt-8 text-[10px] tracking-wider uppercase opacity-60 select-none">Sunshine</p>
          </div>
        </div>
      </div>
    </transition>
  </div>
</template>
<script setup lang="ts">
import { ref, watch, computed } from 'vue';
// `useI18n` may be available at runtime in some setups; declare it for TS to avoid errors
declare const useI18n: any;
import { useRoute, useRouter } from 'vue-router';
import ThemeToggle from '@/ThemeToggle.vue';
import SavingStatus from '@/components/SavingStatus.vue';
import LoginModal from '@/components/LoginModal.vue';
import { http } from '@/http';
import { useAuthStore } from './stores/auth';

const route = useRoute();

const linkClass = (path: string) => {
  const base = 'inline-flex items-center gap-2 px-3 py-1 rounded-md text-brand';
  const active = route.path === path;
  if (active) return base + ' font-semibold bg-primary/20 text-brand';
  return base + ' hover:bg-primary/10';
};
const pageTitle = ref('Dashboard');
const displayTitle = computed(() => {
  // If pageTitle is an i18n key like 'navbar.troubleshoot', call $t from template via global $t
  // We return the key here; template will call $t when necessary using a heuristic there.
  return pageTitle.value;
});
// app bar only; sidebar removed

watch(
  () => route.path,
  (p) => {
    const map: Record<string, string> = {
      '/': 'Dashboard',
      '/applications': 'Applications',
      '/settings': 'Settings',
      '/logs': 'navbar.troubleshoot',
      '/troubleshooting': 'navbar.troubleshoot',
      '/clients': 'clients.nav',
    };
    const v = map[p] || 'Sunshine';
    // If the map value is an i18n key (contains a dot), try to translate it
    if (typeof v === 'string' && v.indexOf('.') !== -1 && typeof useI18n !== 'undefined') {
      // useI18n is not available in this scope, so rely on $t in template — store the key
      pageTitle.value = v;
    } else {
      pageTitle.value = v;
    }
  },
  { immediate: true },
);

const loggedOut = ref(false);

async function logout() {
  const authStore = useAuthStore();
  try {
    await http.post('/api/auth/logout', {}, { validateStatus: () => true });
  } catch (e) {
    console.error('Logout failed:', e);
  }
  authStore.isAuthenticated = false;
  loggedOut.value = true;
}

function refreshPage() {
  window.location.reload();
}
</script>
