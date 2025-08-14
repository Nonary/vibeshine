import { defineStore } from 'pinia'
import { ref } from 'vue'

// Simple auth store that tracks whether a session_token cookie is present.
// We avoid making network calls here (which could trigger redirect behavior)
// and instead detect authentication based on the presence of the session cookie.
export const useAuthStore = defineStore('auth', () => {
  const isAuthenticated = ref(false)
  const ready = ref(false)
  const _listeners = []
  let _pollHandle = null

  function _hasSessionCookie() {
    // HttpOnly session cookies cannot be read from JS (document.cookie), so
    // instead perform a lightweight server validation request. The server
    // will check the cookie and return 200 when the session is valid.
    // This function returns a Promise that resolves to a boolean.
    return fetch('/api/auth/validate', { method: 'GET', credentials: 'same-origin' })
      .then(res => res.ok)
      .catch(() => false)
  }

  // detect will handle either sync or async _hasSessionCookie. It updates
  // isAuthenticated.value only when the computed boolean differs.
  async function detect() {
    try {
      const v = await _hasSessionCookie()
      if (v !== isAuthenticated.value) {
        isAuthenticated.value = v
        // notify listeners on transition to authenticated
        if (v) {
          for (const cb of _listeners) {
            try { cb() } catch (e) { console.error('auth listener error', e) }
          }
        }
      }
    } catch (e) {
      // keep existing state on error
      console.error('auth detect error', e)
    }
  }

  function init({ pollInterval = 500 } = {}) {
    if (ready.value) return
    ready.value = true
    // initial detect
    detect()
    // start lightweight polling to detect login events (cookie set by server)
    _pollHandle = setInterval(detect, pollInterval)
  }

  function stop() {
    if (_pollHandle) {
      clearInterval(_pollHandle)
      _pollHandle = null
    }
  }

  function onLogin(cb) {
    if (typeof cb !== 'function') return
    _listeners.push(cb)
    // if already logged in, invoke immediately (async)
    if (isAuthenticated.value) setTimeout(() => { try { cb() } catch {} }, 0)
    return () => {
      const idx = _listeners.indexOf(cb)
      if (idx !== -1) _listeners.splice(idx, 1)
    }
  }

  return {
    isAuthenticated,
    ready,
    init,
    stop,
    onLogin,
  }
})
