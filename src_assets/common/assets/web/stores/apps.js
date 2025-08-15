import { defineStore } from 'pinia'
import { ref } from 'vue'
import { http } from '@/http.js'

// Centralized store for applications list
export const useAppsStore = defineStore('apps', () => {
  const apps = ref([])

  function setApps(list) {
    apps.value = Array.isArray(list) ? list : []
  }

  // Load apps from server. If force is false and apps already present, returns cached list.
  async function loadApps(force = false) {
    if (apps.value && apps.value.length > 0 && !force) return apps.value
    try {
      const r = await http.get('./api/apps')
      if (r.status !== 200) { setApps([]); return apps.value }
      setApps((r.data && r.data.apps) || [])
    } catch (e) {
      setApps([])
    }
    return apps.value
  }

  return {
    apps,
    setApps,
    loadApps,
  }
})
