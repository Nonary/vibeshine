<template>
  <div class="login-container">
    <div class="login-form">
      <div class="text-center mb-4">
        <img src="/images/logo-sunshine-45.png" height="45" alt="Sunshine" />
        <h1 class="h3 mb-3 fw-normal">
          {{ $t('auth.login_title') }}
        </h1>
      </div>

      <div v-if="!isLoggedIn">
        <div class="mb-3">
          <label for="username" class="form-label">{{ $t('_common.username') }}</label>
          <n-input
            id="username"
            v-model:value="credentials.username"
            type="text"
            name="username"
            autocomplete="username"
            placeholder="Username"
          />
        </div>
        <div class="mb-3">
          <label for="password" class="form-label">{{ $t('_common.password') }}</label>
          <n-input
            id="password"
            v-model:value="credentials.password"
            type="password"
            name="password"
            autocomplete="current-password"
            show-password-on="mousedown"
            placeholder="••••••••"
          />
        </div>
        <n-button type="primary" class="w-full" :loading="loading" @click="login">
          {{ $t('auth.login_sign_in') }}
        </n-button>
        <n-alert v-if="error" type="error" class="mt-3" closable @close="error = ''">
          {{ error }}
        </n-alert>
      </div>
      <div v-else class="text-center space-y-3">
        <n-alert type="success">{{ $t('auth.login_success') }}</n-alert>
        <n-spin :show="true">
          <span class="sr-only">{{ $t('auth.login_loading') }}</span>
        </n-spin>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue';
import { useI18n } from 'vue-i18n';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';
import { NInput, NButton, NAlert, NSpin } from 'naive-ui';

const auth = useAuthStore();
const credentials = ref({ username: '', password: '' });
const loading = ref(false);
const error = ref('');
const isLoggedIn = ref(false);
const requestedRedirect = ref('/');
const safeRedirect = ref('/');
const { t } = useI18n ? useI18n() : { t: (k, d) => d || k };

function sanitizeRedirect(raw) {
  try {
    if (!raw || typeof raw !== 'string') return '/';
    // decode then re-encode for validation of % sequences
    try {
      raw = decodeURIComponent(raw);
    } catch {
      /* ignore */
    }
    // Must start with single slash, no protocol, no double slash at start, limit length
    if (!raw.startsWith('/')) return '/';
    if (raw.startsWith('//')) return '/';
    if (raw.includes('://')) return '/';
    if (raw.length > 512) return '/';
    // Strip any /login recursion to avoid loop
    if (raw.startsWith('/login')) return '/';
    return raw;
  } catch {
    return '/';
  }
}
function redirectNowIfAuthenticated() {
  // Rely on shared Pinia auth state (initialized during app startup)
  if (auth.isAuthenticated) {
    const target = sanitizeRedirect(
      sessionStorage.getItem('pending_redirect') || safeRedirect.value || '/',
    );
    window.location.replace(target);
  }
}

onMounted(() => {
  document.title = `Sunshine - ${t('auth.login_title')}`;
  const urlParams = new URLSearchParams(window.location.search);
  const redirectParam = urlParams.get('redirect');
  if (redirectParam) {
    const sanitized = sanitizeRedirect(redirectParam);
    sessionStorage.setItem('pending_redirect', sanitized);
    urlParams.delete('redirect');
    const cleanUrl =
      window.location.pathname + (urlParams.toString() ? '?' + urlParams.toString() : '');
    window.history.replaceState({}, document.title, cleanUrl);
  }
  requestedRedirect.value = sessionStorage.getItem('pending_redirect') || '/';
  safeRedirect.value = sanitizeRedirect(requestedRedirect.value);

  // The auth store is initialized during app init; just perform redirect check and listen for login events
  redirectNowIfAuthenticated();
  auth.onLogin(() => {
    redirectNowIfAuthenticated();
  });
});

/**
 * Attempt login with credentials.
 * @returns {Promise<void>}
 */
async function login() {
  loading.value = true;
  error.value = '';
  try {
    const response = await http.post(
      '/api/auth/login',
      {
        username: credentials.value.username,
        password: credentials.value.password,
        redirect: requestedRedirect.value,
      },
      { validateStatus: () => true },
    );
    const data = response.data || {};
    if (response.status === 200 && data.status) {
      // Login endpoint created the session cookie; rely on shared auth detection for global state
      isLoggedIn.value = true;
      safeRedirect.value = sanitizeRedirect(data.redirect) || '/';
      sessionStorage.removeItem('pending_redirect');
      // Attempt immediate redirect; auth store will also detect the cookie and notify listeners
      setTimeout(() => {
        redirectToApp();
      }, 250);
    } else {
      error.value = data.error || t('auth.login_failed');
    }
  } catch (e) {
    console.error('Login error:', e);
    error.value = t('auth.login_network_error');
  } finally {
    loading.value = false;
  }
}

/**
 * Redirects the user to the application after successful login.
 * @returns {void}
 */
function redirectToApp() {
  window.location.replace(safeRedirect.value);
}
</script>

<style scoped>
.login-container {
  max-width: 400px;
  margin: 2rem auto;
  padding: 2rem;
}

.login-form {
  background: var(--bs-body-bg);
  border: 1px solid var(--bs-border-color);
  border-radius: 0.375rem;
  padding: 2rem;
}
</style>
