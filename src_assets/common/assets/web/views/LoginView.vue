<template>
  <div
    class="min-h-screen flex items-center justify-center py-12 px-4 sm:px-6 lg:px-8 bg-solar-light dark:bg-lunar-dark text-solar-dark dark:text-lunar-light">
    <div
      class="w-full max-w-md space-y-8 rounded-lg p-8 shadow-sm bg-solar-surface dark:bg-lunar-surface border border-solar-secondary/10 dark:border-lunar-secondary/10">
      <div class="flex flex-col items-center mb-4">
        <img src="/images/logo-sunshine-45.png" height="45" alt="Sunshine" import { useAuthStore }
          from '@/stores/auth.js' import { http } from '@/http.js' class="mb-2">
        <h1 class="text-xl font-semibold text-solar-secondary dark:text-lunar-onLight">
          {{ t('auth.login_title') }}
        </h1>
      </div>

      <form v-if="!isLoggedIn" autocomplete="on" class="space-y-4" @submit.prevent="login">
        <div>
          <label for="username"
            class="block text-sm font-medium text-solar-secondary/80 dark:text-lunar-onLight/80 mb-1">{{
              t('_common.username') }}</label>
          <input id="username" v-model="credentials.username" type="text" name="username" required
            autocomplete="username"
            class="appearance-none block w-full px-3 py-2 rounded-md shadow-sm border border-solar-secondary/20 dark:border-lunar-secondary/20 bg-transparent text-solar-dark dark:text-lunar-light placeholder-solar-secondary/50 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40">
        </div>

        <div>
          <label for="password"
            class="block text-sm font-medium text-solar-secondary/80 dark:text-lunar-onLight/80 mb-1">{{
              t('_common.password') }}</label>
          <input id="password" v-model="credentials.password" type="password" name="password" required
            autocomplete="current-password"
            class="appearance-none block w-full px-3 py-2 rounded-md shadow-sm border border-solar-secondary/20 dark:border-lunar-secondary/20 bg-transparent text-solar-dark dark:text-lunar-light placeholder-solar-secondary/50 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40">
        </div>

        <button type="submit" :disabled="loading"
          class="w-full inline-flex items-center justify-center px-4 py-2 text-sm font-medium rounded-md bg-solar-primary text-solar-onPrimary hover:bg-solar-primary/90 dark:bg-lunar-primary dark:text-lunar-onPrimary disabled:opacity-60 disabled:cursor-not-allowed focus:outline-none focus:ring-2 focus:ring-solar-primary/50 dark:focus:ring-lunar-primary/50">
          <svg v-if="loading" class="animate-spin -ml-1 mr-2 h-4 w-4 text-white" xmlns="http://www.w3.org/2000/svg"
            fill="none" viewBox="0 0 24 24">
            <circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4" />
            <path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8v4a4 4 0 00-4 4H4z" />
          </svg>
          <span>{{ t('auth.login_sign_in') }}</span>
        </button>

        <div v-if="error"
          class="mt-3 text-sm rounded p-2 text-solar-danger bg-solar-danger/10 border border-solar-danger/20 dark:text-lunar-danger dark:bg-lunar-danger/10 dark:border-lunar-danger/20">
          {{ error }}
        </div>
      </form>

      <div v-else class="text-center">
        <div
          class="mt-4 inline-flex items-center px-3 py-2 rounded text-solar-success bg-solar-success/10 border border-solar-success/20 dark:text-lunar-success dark:bg-lunar-success/10 dark:border-lunar-success/20">
          {{ t('auth.login_success') }}
        </div>
        <div class="mt-3">
          <svg class="animate-spin h-6 w-6 text-gray-600 mx-auto" xmlns="http://www.w3.org/2000/svg" fill="none"
            viewBox="0 0 24 24">
            <circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4" />
            <path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8v4a4 4 0 00-4 4H4z" />
          </svg>
          <span class="sr-only">{{ t('auth.login_loading') }}</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAuthStore } from '@/stores/auth.js'
import { http } from '@/http.js'

const auth = useAuthStore()
const credentials = ref({ username: '', password: '' })
const loading = ref(false)
const error = ref('')
const isLoggedIn = ref(false)
const requestedRedirect = ref('/')
const safeRedirect = ref('/')
const { t } = useI18n ? useI18n() : { t: (k, d) => d || k }

function sanitizeRedirect(raw) {
  try {
    if (!raw || typeof raw !== 'string') return '/'
    try { raw = decodeURIComponent(raw) } catch { /* ignore */ }
    if (!raw.startsWith('/')) return '/'
    if (raw.startsWith('//')) return '/'
    if (raw.includes('://')) return '/'
    if (raw.length > 512) return '/'
    if (raw.startsWith('/login')) return '/'
    return raw
  } catch { return '/' }
}

function redirectNowIfAuthenticated() {
  // Prefer shared auth store if available
  try {
    if (auth.isAuthenticated) {
      const target = sanitizeRedirect(sessionStorage.getItem('pending_redirect') || safeRedirect.value || '/')
      window.location.replace(target)
      return
    }
  } catch (e) { }
}

onMounted(() => {
  document.title = `Sunshine - ${t('auth.login_title')}`
  const urlParams = new URLSearchParams(window.location.search)
  const redirectParam = urlParams.get('redirect')
  if (redirectParam) {
    const sanitized = sanitizeRedirect(redirectParam)
    sessionStorage.setItem('pending_redirect', sanitized)
    urlParams.delete('redirect')
    const cleanUrl = window.location.pathname + (urlParams.toString() ? '?' + urlParams.toString() : '')
    window.history.replaceState({}, document.title, cleanUrl)
  }
  requestedRedirect.value = sessionStorage.getItem('pending_redirect') || '/'
  safeRedirect.value = sanitizeRedirect(requestedRedirect.value)
  // auth store is initialized during app init; prefer shared state and listen for login events
  redirectNowIfAuthenticated()
  auth.onLogin(() => {
    redirectNowIfAuthenticated()
  })
})

async function login() {
  loading.value = true
  error.value = ''
  try {
    const response = await http.post('/api/auth/login', {
      username: credentials.value.username,
      password: credentials.value.password,
      redirect: requestedRedirect.value
    }, { validateStatus: () => true })
    const data = response.data || {}
    if (response.status === 200 && data.status) {
      isLoggedIn.value = true
      safeRedirect.value = sanitizeRedirect(data.redirect) || '/'
      sessionStorage.removeItem('pending_redirect')
      setTimeout(() => { redirectToApp() }, 500)
    } else {
      error.value = data.error || t('auth.login_failed')
    }
  } catch (e) {
    console.error('Login error:', e)
    error.value = t('auth.login_network_error')
  } finally {
    loading.value = false
  }
}

function redirectToApp() {
  window.location.replace(safeRedirect.value)
}
</script>

<!-- Styles removed: view now uses Tailwind palette classes defined in tailwind.config.js -->
