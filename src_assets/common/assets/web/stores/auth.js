import { defineStore } from 'pinia'
import { ref } from 'vue'

// Auth store now driven by axios/http interceptor layer instead of polling.
// Provides subscription for login events and a setter used by http.js.
export const useAuthStore = defineStore('auth', () => {
  const isAuthenticated = ref(false)
  const ready = ref(false)
  const _listeners = []

  function setAuthenticated(v) {
    if (v === isAuthenticated.value) return
    const becameAuthed = !isAuthenticated.value && v
    isAuthenticated.value = v
    if (becameAuthed) {
      for (const cb of _listeners) {
        try { cb() } catch (e) { console.error('auth listener error', e) }
      }
    }
  }

  // Single init call invoked during app bootstrap after http layer validation.
  function init() {
    if (ready.value) return
    ready.value = true
  }

  function onLogin(cb) {
    if (typeof cb !== 'function') return
    _listeners.push(cb)
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
    setAuthenticated,
    onLogin,
  }
})
