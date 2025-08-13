<template>
  <div class="min-h-screen flex flex-col bg-solar-light dark:bg-lunar-dark text-solar-dark dark:text-lunar-light">
    <!-- App Bar -->
    <header
      class="h-14 flex items-center gap-4 px-4 border-b border-solar-dark/10 dark:border-lunar-light/10 bg-solar-light/70 dark:bg-lunar-dark/60 backdrop-blur supports-[backdrop-filter]:bg-solar-light/40 supports-[backdrop-filter]:dark:bg-lunar-dark/40">
      <div class="flex items-center gap-3 min-w-0">
        <img src="/images/logo-sunshine-45.png" alt="Sunshine" class="h-8 w-8" />
        <h1 class="text-base md:text-lg font-semibold tracking-tight truncate">{{ displayTitle }}</h1>
      </div>
      <nav class="hidden md:flex items-center gap-1 text-sm font-medium ml-2">
        <RouterLink to="/" class="appbar-link"><i class="fas fa-gauge"></i><span>Dashboard</span></RouterLink>
        <RouterLink to="/applications" class="appbar-link"><i class="fas fa-grid-2"></i><span>Applications</span>
        </RouterLink>
        <RouterLink to="/sessions" class="appbar-link"><i class="fas fa-signal-stream"></i><span>Sessions</span>
        </RouterLink>
        <RouterLink to="/settings" class="appbar-link"><i class="fas fa-sliders"></i><span>Settings</span></RouterLink>
        <RouterLink to="/troubleshooting" class="appbar-link"><i class="fas fa-bug"></i><span>{{
          $t('navbar.troubleshoot') }}</span></RouterLink>
        <RouterLink to="/resources" class="appbar-link"><i class="fas fa-circle-info"></i><span>Resources</span>
        </RouterLink>
      </nav>
      <div class="ml-auto flex items-center gap-3 text-xs">
        <StreamingStatus />
        <ThemeToggle />
      </div>
    </header>

    <!-- Content -->
    <main class="flex-1 overflow-auto p-4 md:p-6 space-y-6">
      <router-view />
    </main>
  </div>
</template>
<script setup>
import { ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import ThemeToggle from '../ThemeToggle.vue'
import StreamingStatus from '../components/StreamingStatus.vue'

const route = useRoute()
import { computed } from 'vue'
const pageTitle = ref('Dashboard')
const displayTitle = computed(() => {
  // If pageTitle is an i18n key like 'navbar.troubleshoot', call $t from template via global $t
  // We return the key here; template will call $t when necessary using a heuristic there.
  return pageTitle.value
})
// app bar only; sidebar removed

watch(() => route.path, (p) => {
  const map = {
    '/': 'Dashboard',
    '/applications': 'Applications',
    '/sessions': 'Streaming Sessions',
    '/settings': 'Settings',
    '/logs': 'navbar.troubleshoot',
    '/troubleshooting': 'navbar.troubleshoot',
    '/resources': 'Resources'
  }
  const v = map[p] || 'Sunshine'
  // If the map value is an i18n key (contains a dot), try to translate it
  if (typeof v === 'string' && v.indexOf('.') !== -1 && typeof useI18n !== 'undefined') {
    // useI18n is not available in this scope, so rely on $t in template — store the key
    pageTitle.value = v
  } else {
    pageTitle.value = v
  }
}, { immediate: true })
</script>
<style scoped>
/* Top app bar link styles */
.appbar-link {
  display: inline-flex;
  align-items: center;
  gap: .5rem;
  padding: .375rem .625rem;
  border-radius: .5rem;
  color: rgba(0, 0, 0, .75);
}

.dark .appbar-link {
  color: rgba(245, 249, 255, .75);
}

.appbar-link:hover {
  background: rgba(253, 184, 19, .15);
  color: #0D3B66;
}

.dark .appbar-link:hover {
  background: rgba(77, 163, 255, .18);
  color: #F5F9FF;
}

.appbar-link.router-link-active {
  background: rgba(253, 184, 19, .22);
  color: #0D3B66;
  font-weight: 600
}

.dark .appbar-link.router-link-active {
  background: rgba(77, 163, 255, .22);
  color: #F5F9FF;
  font-weight: 600
}
</style>
