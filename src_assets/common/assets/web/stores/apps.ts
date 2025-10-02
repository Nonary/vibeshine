import { defineStore } from 'pinia';
import { ref, Ref } from 'vue';
import { http } from '@/http';

export interface PrepCmd {
  do?: string;
  undo?: string;
  elevated?: boolean;
}

export interface App {
  name?: string;
  output?: string;
  cmd?: string | string[];
  uuid?: string;
  'working-dir'?: string;
  'image-path'?: string;
  'exclude-global-prep-cmd'?: boolean;
  'exclude-global-state-cmd'?: boolean;
  elevated?: boolean;
  'auto-detach'?: boolean;
  'wait-all'?: boolean;
  'frame-gen-limiter-fix'?: boolean;
  'gen1-framegen-fix'?: boolean;
  'gen2-framegen-fix'?: boolean;
  'exit-timeout'?: number;
  'prep-cmd'?: PrepCmd[];
  'state-cmd'?: PrepCmd[];
  detached?: string[];
  'allow-client-commands'?: boolean;
  'terminate-on-pause'?: boolean;
  'virtual-display'?: boolean;
  'use-app-identity'?: boolean;
  'per-client-app-identity'?: boolean;
  'scale-factor'?: number;
  gamepad?: string;
  'lossless-scaling-framegen'?: boolean;
  'lossless-scaling-target-fps'?: number;
  'lossless-scaling-rtss-limit'?: number;
  'lossless-scaling-profile'?: string;
  'lossless-scaling-recommended'?: Record<string, unknown>;
  'lossless-scaling-custom'?: Record<string, unknown>;
  // Fallback for any other server fields we don't model yet
  [key: string]: any;
}

interface AppsResponse {
  apps?: App[];
  current_app?: string;
  host_name?: string;
  host_uuid?: string;
}

// Centralized store for applications list
export const useAppsStore = defineStore('apps', () => {
  const apps: Ref<App[]> = ref([]);
  const currentAppUuid: Ref<string> = ref('');
  const hostName: Ref<string> = ref('');
  const hostUuid: Ref<string> = ref('');

  function setApps(
    list: App[],
    meta?: { current_app?: string; host_name?: string; host_uuid?: string },
  ): void {
    apps.value = Array.isArray(list) ? list : [];
    currentAppUuid.value = typeof meta?.current_app === 'string' ? meta.current_app : '';
    hostName.value = typeof meta?.host_name === 'string' ? meta.host_name : '';
    hostUuid.value = typeof meta?.host_uuid === 'string' ? meta.host_uuid : '';
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
      const data = r.data || {};
      const list = Array.isArray(data.apps) ? (data.apps as App[]) : [];
      setApps(list, {
        current_app: typeof data.current_app === 'string' ? data.current_app : '',
        host_name: typeof data.host_name === 'string' ? data.host_name : '',
        host_uuid: typeof data.host_uuid === 'string' ? data.host_uuid : '',
      });
    } catch (e) {
      setApps([]);
    }
    return apps.value;
  }

  async function launch(uuid: string): Promise<void> {
    await http.post('./api/apps/launch', { uuid }, { headers: { 'Content-Type': 'application/json' } });
  }

  async function close(): Promise<void> {
    await http.post('./api/apps/close', {}, { headers: { 'Content-Type': 'application/json' } });
  }

  async function reorder(order: string[]): Promise<void> {
    await http.post('./api/apps/reorder', { order }, { headers: { 'Content-Type': 'application/json' } });
  }

  return {
    apps,
    setApps,
    loadApps,
    launch,
    close,
    reorder,
    currentAppUuid,
    hostName,
    hostUuid,
  };
});
