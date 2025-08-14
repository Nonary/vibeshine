// Axios HTTP client with centralized auth handling
import axios from 'axios'
import { useAuthStore } from './stores/auth.js'

// Create a singleton axios instance
export const http = axios.create({
  // baseURL left relative so it works behind reverse proxies
  withCredentials: true,
  headers: {
    'X-Requested-With': 'XMLHttpRequest'
  }
})

let authInitialized = false

function initAuthHandling() {
  if (authInitialized) return
  authInitialized = true
  const auth = useAuthStore()

  // Response interceptor to detect auth changes
  http.interceptors.response.use(
    async (response) => {
      if (response?.status === 401) {
        // Update store state if we thought we were authenticated
        if (auth.isAuthenticated) auth.setAuthenticated(false)
        // Let existing fetch-based redirect (init.js) handle navigation, or we could replicate here
      } else if (response?.config && !auth.isAuthenticated) {
        // For any successful API response, we can optimistically set auth true
        // But only after we validated once via /api/auth/validate during init
        // This avoids flicker on first load before validation
      }
      return response
    },
    async (error) => {
      if (error?.response?.status === 401) {
        if (auth.isAuthenticated) auth.setAuthenticated(false)
      }
      return Promise.reject(error)
    }
  )
}

export async function validateSessionOnce() {
  const auth = useAuthStore()
  try {
    const res = await http.get('/api/auth/validate', { validateStatus: () => true })
    auth.setAuthenticated(!!res && res.status === 200)
  } catch {
    auth.setAuthenticated(false)
  }
}

// Called from main init after pinia is ready
export function initHttpLayer() {
  initAuthHandling()
  return validateSessionOnce()
}
