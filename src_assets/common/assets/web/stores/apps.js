import { defineStore } from 'pinia'
import { ref } from 'vue'

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
      const r = await fetch('./api/apps')
      if (!r.ok) { setApps([]); return apps.value }
      const json = await r.json()
      setApps(json.apps || [])
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
