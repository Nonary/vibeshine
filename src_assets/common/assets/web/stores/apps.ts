import { defineStore } from 'pinia';
import { ref, Ref } from 'vue';
import { http } from '@/http';

interface App {
  [key: string]: any; // Define more specific types as needed
}

interface AppsResponse {
  apps?: App[];
}

// Centralized store for applications list
export const useAppsStore = defineStore('apps', () => {
  const apps: Ref<App[]> = ref([]);

  function setApps(list: App[]): void {
    apps.value = Array.isArray(list) ? list : [];
  }

  // Load apps from server. If force is false and apps already present, returns cached list.
  async function loadApps(force = false): Promise<App[]> {
    if (apps.value && apps.value.length > 0 && !force) return apps.value;
    try {
      const r = await http.get<AppsResponse>('./api/apps');
      if (r.status !== 200) {
        setApps([]);
        return apps.value;
      }
      setApps((r.data && r.data.apps) || []);
    } catch (e) {
      setApps([]);
    }
    return apps.value;
  }

  return {
    apps,
    setApps,
    loadApps,
  };
});
