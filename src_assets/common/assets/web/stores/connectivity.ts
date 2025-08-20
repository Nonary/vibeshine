import { defineStore } from 'pinia';
import { ref, Ref } from 'vue';
import { http } from '@/http';

// Connectivity heartbeat + global offline state
export const useConnectivityStore = defineStore('connectivity', () => {
  const offline: Ref<boolean> = ref(false);
  const checking: Ref<boolean> = ref(false);
  const lastOk: Ref<number | null> = ref(null);
  const retryMs: Ref<number> = ref(3000);
  const started: Ref<boolean> = ref(false);
  let intervalId: number | null = null;

  function setOffline(v: boolean): void {
    if (offline.value === v) return;
    offline.value = v;
  }

  async function checkOnce(): Promise<void> {
    if (checking.value) return;
    checking.value = true;
    try {
      // Any HTTP response (including 401/4xx/5xx) indicates server is reachable
      const res = await http.get('/api/metadata', {
        validateStatus: () => true,
        timeout: 2500,
      });
      if (res) {
        const wasOffline = offline.value;
        setOffline(false);
        lastOk.value = Date.now();
        // If we just recovered, refresh the page to restore app state
        if (wasOffline) {
          // Small delay to let UI update the message before refresh
          setTimeout(() => {
            try {
              window.location.reload();
            } catch {}
          }, 300);
        }
      }
    } catch (e) {
      setOffline(true);
    } finally {
      checking.value = false;
    }
  }

  function start(): void {
    if (started.value) return;
    started.value = true;

    // Initial check shortly after mount
    setTimeout(() => checkOnce(), 500);

    // Heartbeat interval
    intervalId = window.setInterval(() => {
      checkOnce();
    }, retryMs.value);

    // Browser online/offline events reflect network, not server, but still useful
    window.addEventListener('online', () => {
      // On browser regaining network, check server reachability immediately
      setTimeout(() => checkOnce(), 200);
    });
    window.addEventListener('offline', () => setOffline(true));

    // Listen to HTTP-layer signals for faster reaction on failures
    window.addEventListener('sunshine:offline', () => setOffline(true));
    window.addEventListener('sunshine:online', () => {
      // Do not force reload here; let heartbeat handle the recovery
      setOffline(false);
      lastOk.value = Date.now();
    });
  }

  function stop(): void {
    if (intervalId) {
      clearInterval(intervalId);
      intervalId = null;
    }
    started.value = false;
  }

  return {
    offline,
    checking,
    lastOk,
    retryMs,
    start,
    stop,
    checkOnce,
  };
});

