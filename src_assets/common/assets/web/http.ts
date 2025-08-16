// Axios HTTP client with centralized auth handling
import axios, { AxiosResponse, AxiosError } from 'axios';
import { useAuthStore } from '@/stores/auth';

// Create a singleton axios instance
export const http = axios.create({
  // baseURL left relative so it works behind reverse proxies
  withCredentials: true,
  headers: {
    'X-Requested-With': 'XMLHttpRequest',
  },
});

let authInitialized = false;

function initAuthHandling(): void {
  if (authInitialized) return;
  authInitialized = true;
  const auth = useAuthStore();

  function sanitizePath(path: string): string {
    try {
      if (typeof path !== 'string') return '/';
      if (!path.startsWith('/')) return '/';
      if (path.startsWith('//')) return '/';
      if (path.includes('://')) return '/';
      if (path.length > 512) return '/';
      return path;
    } catch {
      return '/';
    }
  }

  function triggerLoginModal(): void {
    if (typeof window === 'undefined') return;
    try {
      const current = sanitizePath(
        window.location.pathname + window.location.search + window.location.hash,
      );
      auth.requireLogin(current);
    } catch {
      /* noop */
    }
  }

  // Response interceptor to detect auth changes
  http.interceptors.response.use(
    async (response: AxiosResponse) => {
      if (response?.status === 401) {
        if (auth.isAuthenticated) auth.setAuthenticated(false);
        triggerLoginModal();
      }
      return response;
    },
    async (error: AxiosError) => {
      if (error?.response?.status === 401) {
        if (auth.isAuthenticated) auth.setAuthenticated(false);
        triggerLoginModal();
      } else if (
        error?.response?.status === 400 &&
        error?.response?.data &&
        /Credentials not configured/i.test(JSON.stringify(error.response.data))
      ) {
        // Backend indicates no credentials configured yet
        auth.setCredentialsConfigured(false);
        triggerLoginModal();
      }
      return Promise.reject(error);
    },
  );
}

// Called from main init after pinia is ready
export function initHttpLayer(): void {
  initAuthHandling();
}
