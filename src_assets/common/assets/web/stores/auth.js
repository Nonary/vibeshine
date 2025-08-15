import { defineStore } from 'pinia';
import { ref } from 'vue';
import { http } from '@/http.js';

// Auth store now driven by axios/http interceptor layer instead of polling.
// Provides subscription for login events and a setter used by http.js.
export const useAuthStore = defineStore('auth', () => {
  const isAuthenticated = ref(false);
  const ready = ref(false);
  const _listeners = [];
  const pendingRedirect = ref('/');
  const showLoginModal = ref(false);
  const credentialsConfigured = ref(true);
  const loggingIn = ref(false);

  function setAuthenticated(v) {
    if (v === isAuthenticated.value) return;
    const becameAuthed = !isAuthenticated.value && v;
    isAuthenticated.value = v;
    if (becameAuthed) {
      for (const cb of _listeners) {
        try {
          cb();
        } catch (e) {
          console.error('auth listener error', e);
        }
      }
    }
  }

  // Single init call invoked during app bootstrap after http layer validation.
  async function init() {
    if (ready.value) return;
    try {
      const res = await http.get('/api/auth/status', { validateStatus: () => true });
      if (res && res.status === 200 && res.data) {
        if (typeof res.data.credentials_configured === 'boolean') {
          credentialsConfigured.value = res.data.credentials_configured;
        }
        if (res.data.authenticated) {
          setAuthenticated(true);
        }
        // Show login only if explicitly required and not already authenticated
        if (res.data.login_required && !res.data.authenticated) {
          showLoginModal.value = true;
        }
      }
    } catch (e) {
      // Network issues; defer to interceptor behavior
    } finally {
      ready.value = true;
    }
  }

  function onLogin(cb) {
    if (typeof cb !== 'function') return;
    _listeners.push(cb);
    if (isAuthenticated.value)
      setTimeout(() => {
        try {
          cb();
        } catch {}
      }, 0);
    return () => {
      const idx = _listeners.indexOf(cb);
      if (idx !== -1) _listeners.splice(idx, 1);
    };
  }

  function requireLogin(redirectPath) {
    if (redirectPath && typeof redirectPath === 'string') {
      pendingRedirect.value = redirectPath;
    }
    showLoginModal.value = true;
  }

  function hideLogin() {
    showLoginModal.value = false;
  }

  function setCredentialsConfigured(v) {
    credentialsConfigured.value = !!v;
  }

  return {
    isAuthenticated,
    ready,
    init,
    setAuthenticated,
    onLogin,
    pendingRedirect,
    showLoginModal,
    requireLogin,
    hideLogin,
    credentialsConfigured,
    setCredentialsConfigured,
    loggingIn,
  };
});
