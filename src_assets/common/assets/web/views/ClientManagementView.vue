<template>
  <div class="px-4 pb-12 space-y-8">
    <h1 class="text-2xl font-semibold pt-6 flex items-center gap-3 text-brand">
      <i class="fas fa-users-cog" /> {{ $t('clients.title') }}
    </h1>

    <n-card :segmented="{ content: true, footer: false }" class="space-y-4">
      <template #header>
        <h2 class="text-lg font-medium flex items-center gap-2">
          <i class="fas fa-link" /> {{ $t('clients.pair_title') }}
        </h2>
      </template>

      <n-tabs v-model:value="activePairTab" type="segment" size="small">
        <n-tab-pane name="otp" :tab="$t('pin.otp_pairing')">
          <form class="space-y-6" @submit.prevent="requestOtp">
            <div class="grid gap-6 md:grid-cols-2">
              <div class="space-y-4">
                <n-form label-placement="top" class="space-y-4">
                  <n-form-item :label="$t('pin.otp_passphrase')">
                    <n-input
                      v-model:value="otpPassphrase"
                      :placeholder="$t('pin.otp_passphrase')"
                      :input-props="{ pattern: '[0-9a-zA-Z]{4,}', required: true, autocapitalize: 'none' }"
                    />
                  </n-form-item>
                  <n-form-item :label="$t('pin.device_name')">
                    <n-input
                      v-model:value="otpDeviceName"
                      :placeholder="$t('pin.device_name')"
                      :input-props="{ autocapitalize: 'none' }"
                    />
                  </n-form-item>
                </n-form>

                <div class="space-y-3">
                  <div class="text-xs opacity-70 flex items-center gap-2" v-if="deepLink">
                    <span class="truncate">{{ deepLink }}</span>
                    <n-button text size="tiny" @click="toggleHostEditing">
                      <i class="fas" :class="editingHost ? 'fa-times' : 'fa-pen-to-square'" />
                      <span class="ml-1">{{ editingHost ? $t('_common.dismiss') : 'Edit link' }}</span>
                    </n-button>
                  </div>

                  <transition name="fade-fast">
                    <div v-if="editingHost" class="grid gap-3 md:grid-cols-2">
                      <n-form-item label="HOST">
                        <n-input v-model:value="hostAddr" placeholder="hostname or IP" />
                      </n-form-item>
                      <n-form-item label="PORT">
                        <n-input v-model:value="hostPort" placeholder="47984" :input-props="{ inputmode: 'numeric' }" />
                      </n-form-item>
                      <div class="md:col-span-2 flex justify-end gap-2">
                        <n-button tertiary size="small" @click="cancelHostEdit">{{ $t('_common.cancel') }}</n-button>
                        <n-button type="primary" size="small" :disabled="!canSaveHost" @click="saveHost">
                          {{ $t('_common.save') }}
                        </n-button>
                      </div>
                    </div>
                  </transition>
                </div>
              </div>

              <div class="space-y-4 flex flex-col items-center justify-center">
                <div class="text-5xl font-mono tracking-[0.4em] text-center select-all">
                  {{ otpDisplay }}
                </div>
                <div
                  ref="qrContainerRef"
                  class="min-h-[192px] min-w-[192px] flex items-center justify-center bg-light border border-dark/10 dark:bg-surface dark:border-light/10 rounded-2xl p-4"
                ></div>
                <n-alert v-if="otpMessage" :type="otpStatus || 'warning'">{{ otpMessage }}</n-alert>
              </div>
            </div>

            <div class="flex justify-end">
              <n-button type="primary" :loading="otpBusy" :disabled="editingHost" attr-type="submit">
                <i class="fas fa-rotate mr-2" />
                {{ $t('pin.generate_pin') }}
              </n-button>
            </div>
          </form>
          <n-alert class="mt-4" type="info">{{ $t('pin.otp_msg') }}</n-alert>
        </n-tab-pane>

        <n-tab-pane name="pin" :tab="$t('pin.pin_pairing')">
          <n-form class="grid grid-cols-1 md:grid-cols-3 gap-4 items-end" @submit.prevent="registerDevice">
            <n-form-item class="flex flex-col" :label="$t('navbar.pin')" label-placement="top">
              <n-input
                id="pin-input"
                v-model:value="pin"
                :placeholder="$t('navbar.pin')"
                :input-props="{
                  inputmode: 'numeric',
                  pattern: '^[0-9]{4}$',
                  title: 'Enter 4 digits',
                  maxlength: 4,
                  required: true,
                }"
              />
            </n-form-item>
            <n-form-item class="flex flex-col" :label="$t('pin.device_name')" label-placement="top">
              <n-input
                id="name-input"
                v-model:value="deviceName"
                :placeholder="$t('pin.device_name')"
                :input-props="{ autocapitalize: 'none' }"
              />
            </n-form-item>
            <n-form-item class="flex flex-col md:items-end">
              <n-button
                :disabled="pairing"
                class="w-full md:w-auto"
                type="primary"
                attr-type="submit"
              >
                <span v-if="!pairing">{{ $t('pin.send') }}</span>
                <span v-else>{{ $t('clients.pairing') }}</span>
              </n-button>
            </n-form-item>
          </n-form>
          <div class="mt-4 space-y-2">
            <n-alert v-if="pairStatus === true" type="success">{{ $t('pin.pair_success') }}</n-alert>
            <n-alert v-if="pairStatus === false" type="error">{{ $t('pin.pair_failure') }}</n-alert>
          </div>
        </n-tab-pane>
      </n-tabs>

      <n-alert type="warning" :title="$t('_common.warning')" class="text-sm">
        {{ $t('pin.warning_msg') }}
      </n-alert>
    </n-card>

    <n-card :segmented="{ content: true, footer: false }" class="space-y-4">
      <template #header>
        <div class="flex items-center gap-2">
          <i class="fas fa-users" />
          <h2 class="text-lg font-medium">{{ $t('clients.existing_title') }}</h2>
        </div>
      </template>

      <div class="flex flex-wrap items-center gap-3">
        <n-button
          class="ml-auto"
          type="error"
          strong
          :disabled="unpairAllPressed || clients.length === 0"
          @click="askConfirmUnpairAll"
        >
          <i class="fas fa-user-slash" />
          {{ $t('troubleshooting.unpair_all') }}
        </n-button>
      </div>
      <p class="text-sm opacity-75">{{ $t('pin.device_management_desc') }}</p>
      <p class="text-sm opacity-75">
        {{ $t('pin.device_management_warning') }}
        <a
          href="https://github.com/ClassicOldSong/Apollo/wiki/Permission-System"
          class="underline"
          target="_blank"
          rel="noopener"
        >
          {{ $t('_common.learn_more') }}
        </a>
      </p>

      <n-alert v-if="singleUnpairStatus === 'success'" type="success" class="mb-2">
        {{ $t('pin.unpair_single_success') }}
      </n-alert>
      <n-alert v-if="unpairAllStatus === true" type="success" class="mb-3">{{
        $t('pin.unpair_all_success')
      }}</n-alert>
      <n-alert v-if="unpairAllStatus === false" type="error" class="mb-3">{{
        $t('pin.unpair_all_error')
      }}</n-alert>

      <div v-if="clients.length > 0" class="space-y-4">
        <div
          v-for="client in clients"
          :key="client.uuid"
          class="border border-dark/10 dark:border-light/10 rounded-2xl p-4 space-y-4"
        >
          <div class="flex flex-wrap gap-3 items-center justify-between">
            <div class="flex items-center gap-3 min-w-0">
              <n-tag :type="permissionBadgeType(clientDisplayPerm(client))">
                [ {{ permToStr(clientDisplayPerm(client)) }} ]
              </n-tag>
              <div class="min-w-0">
                <div class="font-medium truncate">
                  {{ client.name || $t('troubleshooting.unpair_single_unknown') }}
                </div>
                <div class="text-xs opacity-60 break-all">{{ client.uuid }}</div>
              </div>
            </div>
            <div class="flex items-center gap-2">
              <n-tag v-if="client.connected" type="warning" size="small">
                <i class="fas fa-bolt mr-1" /> Connected
              </n-tag>
              <n-button
                v-if="client.connected"
                tertiary
                size="small"
                @click="disconnectClient(client.uuid)"
              >
                <i class="fas fa-link-slash" />
              </n-button>
              <n-button
                v-if="!client.editing"
                secondary
                size="small"
                @click="startEdit(client)"
              >
                <i class="fas fa-edit" />
              </n-button>
              <n-button
                v-else
                tertiary
                size="small"
                @click="cancelEdit(client)"
              >
                <i class="fas fa-times" />
              </n-button>
              <n-button
                type="error"
                strong
                size="small"
                :loading="!!removing[client.uuid]"
                @click="askConfirmUnpair(client.uuid)"
              >
                <i class="fas fa-trash" />
              </n-button>
            </div>
          </div>

          <transition name="fade-fast">
            <div v-if="client.editing && client.edit" class="space-y-6">
              <n-form label-placement="top" class="space-y-4">
                <div class="grid gap-4 md:grid-cols-2">
                  <n-form-item :label="$t('pin.device_name')">
                    <n-input v-model:value="client.edit.name" autocomplete="off" />
                  </n-form-item>
                  <n-form-item :label="$t('pin.display_mode_override')">
                    <n-input
                      v-model:value="client.edit.displayMode"
                      placeholder="1920x1080x59.94"
                      autocomplete="off"
                    />
                    <template #feedback>
                      <div class="text-xs opacity-70">
                        {{ $t('pin.display_mode_override_desc') }}
                        <a
                          href="https://github.com/ClassicOldSong/Apollo/wiki/Display-Mode-Override"
                          target="_blank"
                          rel="noopener"
                          class="underline"
                        >
                          {{ $t('_common.learn_more') }}
                        </a>
                      </div>
                    </template>
                  </n-form-item>
                </div>
              </n-form>

              <div class="grid gap-4 md:grid-cols-3">
                <div v-for="group in permissionGroups" :key="group.name" class="space-y-2">
                  <div class="text-xs uppercase font-semibold tracking-wide opacity-70">
                    {{ group.label }}
                  </div>
                  <div class="grid gap-2">
                    <n-button
                      v-for="perm in group.permissions"
                      :key="perm.name"
                      size="tiny"
                      :type="permissionButtonType(client.edit.perm, perm.name, perm.suppressed_by)"
                      :disabled="isSuppressed(client.edit.perm, perm.name, perm.suppressed_by)"
                      @click="togglePermission(client, perm.name)"
                    >
                      {{ $t(`permissions.${perm.name}`) }}
                    </n-button>
                  </div>
                </div>
              </div>

              <div class="space-y-4">
                <Checkbox
                  class="block"
                  id="enable_legacy_ordering"
                  label="pin.enable_legacy_ordering"
                  desc="pin.enable_legacy_ordering_desc"
                  v-model="client.edit.enableLegacyOrdering"
                  default="true"
                />

                <Checkbox
                  v-if="platform === 'windows'"
                  class="block"
                  id="always_use_virtual_display"
                  label="pin.always_use_virtual_display"
                  desc="pin.always_use_virtual_display_desc"
                  v-model="client.edit.alwaysUseVirtualDisplay"
                  default="false"
                />

                <Checkbox
                  class="block"
                  id="allow_client_commands"
                  label="pin.allow_client_commands"
                  desc="pin.allow_client_commands_desc"
                  v-model="client.edit.allowClientCommands"
                  default="true"
                />
              </div>

              <div
                v-for="section in commandSections"
                :key="section.key"
                v-if="client.edit.allowClientCommands === 'true' || client.edit.allowClientCommands === true"
                class="space-y-3"
              >
                <div class="text-sm font-medium">{{ $t(section.label) }}</div>
                <div class="text-xs opacity-70">
                  {{ $t(section.desc) }}
                  <a
                    href="https://github.com/ClassicOldSong/Apollo/wiki/Client-Commands"
                    target="_blank"
                    rel="noopener"
                    class="underline"
                  >
                    {{ $t('_common.learn_more') }}
                  </a>
                </div>
                <div class="space-y-2">
                  <div
                    v-for="(cmd, idx) in client.edit[section.key]"
                    :key="`${section.key}-${idx}`"
                    class="flex flex-col md:flex-row md:items-center gap-2"
                  >
                    <n-input v-model:value="cmd.cmd" class="font-mono flex-1" />
                    <n-checkbox
                      v-if="platform === 'windows'"
                      v-model:checked="cmd.elevated"
                      :checked-value="'true'"
                      :unchecked-value="'false'"
                    >
                      {{ $t('_common.elevated') }}
                    </n-checkbox>
                    <div class="flex gap-2">
                      <n-button
                        type="error"
                        quaternary
                        size="small"
                        @click="removeCommand(client, section.key, idx)"
                      >
                        <i class="fas fa-trash" />
                      </n-button>
                      <n-button
                        type="primary"
                        quaternary
                        size="small"
                        @click="addCommand(client, section.key, idx)"
                      >
                        <i class="fas fa-plus" />
                      </n-button>
                    </div>
                  </div>
                </div>
                <n-button
                  size="small"
                  tertiary
                  type="primary"
                  class="mt-1"
                  @click="addCommand(client, section.key, -1)"
                >
                  + {{ $t('config.add') }}
                </n-button>
              </div>

              <div class="flex justify-end gap-3">
                <n-button tertiary @click="cancelEdit(client)">{{ $t('_common.cancel') }}</n-button>
                <n-button type="primary" @click="saveClient(client)">
                  <i class="fas fa-check mr-2" />
                  {{ $t('_common.save') }}
                </n-button>
              </div>
            </div>
          </transition>
        </div>
      </div>
      <div v-else class="p-6 text-center italic opacity-70">
        {{ $t('troubleshooting.unpair_single_no_devices') }}
      </div>
    <n-modal :show="showConfirmRemove" @update:show="(v) => (showConfirmRemove = v)">
      <n-card
        :title="
          $t('clients.confirm_remove_title_named', {
            name: pendingRemoveName || $t('troubleshooting.unpair_single_unknown'),
          })
        "
        style="max-width: 32rem; width: 100%"
      >
        <template #default>
          <p class="text-sm opacity-75">
            {{
              $t('clients.confirm_remove_message_named', {
                name: pendingRemoveName || $t('troubleshooting.unpair_single_unknown'),
              })
            }}
          </p>
        </template>
        <template #footer>
          <div class="w-full flex items-center justify-center gap-3">
            <n-button type="default" strong @click="showConfirmRemove = false">{{
              $t('_common.cancel')
            }}</n-button>
            <n-button type="error" strong @click="confirmRemove">{{
              $t('clients.confirm_remove_action')
            }}</n-button>
          </div>
        </template>
      </n-card>
    </n-modal>

    <n-modal :show="showConfirmUnpairAll" @update:show="(v) => (showConfirmUnpairAll = v)">
      <n-card style="max-width: 28rem" :title="$t('troubleshooting.unpair_all')">
        <template #default>
          <p class="text-sm opacity-75">{{ $t('clients.confirm_unpair_all_message') }}</p>
        </template>
        <template #footer>
          <div class="w-full flex items-center justify-center gap-3">
            <n-button type="default" strong @click="showConfirmUnpairAll = false">{{
              $t('_common.cancel')
            }}</n-button>
            <n-button secondary @click="confirmUnpairAll">{{
              $t('troubleshooting.unpair_all')
            }}</n-button>
          </div>
        </template>
      </n-card>
    </n-modal>
  </div>
</template>

<script setup lang="ts">
import {
  ref,
  reactive,
  computed,
  onMounted,
  onBeforeUnmount,
  nextTick,
  shallowRef,
  watch,
} from 'vue';
import { useI18n } from 'vue-i18n';
import {
  NCard,
  NButton,
  NAlert,
  NModal,
  NInput,
  NForm,
  NFormItem,
  NTabs,
  NTabPane,
  NTag,
  NCheckbox,
  useMessage,
} from 'naive-ui';
import ApiTokenManager from '@/ApiTokenManager.vue';
import TrustedDevicesCard from '@/components/TrustedDevicesCard.vue';
import Checkbox from '@/Checkbox.vue';
import { http } from '@/http';
import { useAuthStore } from '@/stores/auth';

interface CommandEntry {
  cmd: string;
  elevated?: string | boolean;
}

interface RawClientInfo {
  uuid: string;
  name: string;
  perm: string | number;
  display_mode?: string;
  connected?: boolean;
  allow_client_commands?: string | boolean;
  enable_legacy_ordering?: string | boolean;
  always_use_virtual_display?: string | boolean;
  do?: CommandEntry[];
  undo?: CommandEntry[];
}

interface ClientsListResponse {
  status?: boolean;
  platform?: string;
  named_certs?: RawClientInfo[];
}

interface ClientEditState {
  name: string;
  perm: number;
  displayMode: string;
  allowClientCommands: string | boolean;
  enableLegacyOrdering: string | boolean;
  alwaysUseVirtualDisplay: string | boolean;
  doCmds: CommandEntry[];
  undoCmds: CommandEntry[];
}

interface ClientRecord {
  uuid: string;
  name: string;
  perm: number;
  display_mode: string;
  connected: boolean;
  allow_client_commands: string | boolean;
  enable_legacy_ordering: string | boolean;
  always_use_virtual_display: string | boolean;
  do: CommandEntry[];
  undo: CommandEntry[];
  editing: boolean;
  edit?: ClientEditState;
}

declare global {
  interface Window {
    QRCode?: any;
  }
}

const { t } = useI18n();
const message = useMessage();
const authStore = useAuthStore();

// Pairing tabs
const activePairTab = ref<'otp' | 'pin'>(typeof window !== 'undefined' && window.location.hash === '#PIN' ? 'pin' : 'otp');
const hashHandler = () => {
  if (typeof window === 'undefined') return;
  activePairTab.value = window.location.hash === '#PIN' ? 'pin' : 'otp';
};

if (typeof window !== 'undefined') {
  window.addEventListener('hashchange', hashHandler);
}

onBeforeUnmount(() => {
  if (typeof window !== 'undefined') window.removeEventListener('hashchange', hashHandler);
  if (otpResetTimer) window.clearTimeout(otpResetTimer);
});

watch(activePairTab, (tab) => {
  if (typeof window === 'undefined') return;
  window.location.hash = tab === 'pin' ? 'PIN' : 'OTP';
});

// PIN pairing state
const pin = ref('');
const deviceName = ref('');
const pairing = ref(false);
const pairStatus = ref<boolean | null>(null);

// OTP pairing state
const otpPassphrase = ref('');
const otpDeviceName = ref('');
const otpValue = ref('');
const otpMessage = ref('');
const otpStatus = ref<'success' | 'warning' | 'error' | null>(null);
const otpBusy = ref(false);
const hostAddr = ref('');
const hostPort = ref('');
const hostName = ref('');
const editingHost = ref(false);
const hostManuallySet = ref(false);
const qrContainerRef = ref<HTMLDivElement | null>(null);
const qrInstance = shallowRef<any | null>(null);
let otpResetTimer: number | null = null;
let qrLoaderPromise: Promise<any> | null = null;

const otpDisplay = computed(() => (otpValue.value ? otpValue.value : '????'));

const deepLink = computed(() => {
  if (!hostAddr.value || !hostPort.value || !otpValue.value || !otpPassphrase.value) return '';
  const query = new URLSearchParams({
    pin: otpValue.value,
    passphrase: otpPassphrase.value,
    name: hostName.value || '',
  });
  return encodeURI(`art://${hostAddr.value}:${hostPort.value}?${query.toString()}`);
});

const canSaveHost = computed(() => hostAddr.value.trim().length > 0 && hostPort.value.trim().length > 0);

function loadHostCache(): void {
  if (typeof window === 'undefined') return;
  try {
    const raw = window.sessionStorage.getItem('hostInfo');
    if (raw) {
      const parsed = JSON.parse(raw) as { hostAddr?: string; hostPort?: string };
      if (parsed.hostAddr) hostAddr.value = parsed.hostAddr;
      if (parsed.hostPort) hostPort.value = parsed.hostPort;
      hostManuallySet.value = true;
    }
  } catch {
    /* ignore */
  }
}

function saveHostCache(manual = false): void {
  if (typeof window === 'undefined') return;
  const payload = { hostAddr: hostAddr.value, hostPort: hostPort.value };
  if (manual) {
    try {
      window.sessionStorage.setItem('hostInfo', JSON.stringify(payload));
      hostManuallySet.value = true;
    } catch {
      /* ignore */
    }
  }
}

function toggleHostEditing(): void {
  if (editingHost.value) {
    cancelHostEdit();
  } else {
    loadHostCache();
    editingHost.value = true;
  }
}

function cancelHostEdit(): void {
  editingHost.value = false;
  loadHostCache();
}

function saveHost(): void {
  if (!canSaveHost.value) return;
  saveHostCache(true);
  editingHost.value = false;
  void updateQrCode();
}

async function loadQrLibrary(): Promise<any> {
  if (typeof window === 'undefined') return null;
  if (window.QRCode) return window.QRCode;
  if (!qrLoaderPromise) {
    qrLoaderPromise = new Promise((resolve, reject) => {
      const script = document.createElement('script');
      script.src = '/assets/js/qrcode.min.js';
      script.async = true;
      script.onload = () => {
        if (window.QRCode) resolve(window.QRCode);
        else reject(new Error('QRCode library unavailable'));
      };
      script.onerror = () => reject(new Error('Failed to load QRCode library'));
      document.head.appendChild(script);
    }).catch((err) => {
      qrLoaderPromise = null;
      throw err;
    });
  }
  return qrLoaderPromise;
}

async function updateQrCode(): Promise<void> {
  if (!deepLink.value || typeof window === 'undefined') return;
  try {
    const QRCode = await loadQrLibrary();
    if (!QRCode) return;
    await nextTick();
    const container = qrContainerRef.value;
    if (!container) return;
    if (!qrInstance.value) {
      container.innerHTML = '';
      const inner = document.createElement('div');
      inner.className = 'bg-white p-2 rounded-xl';
      container.appendChild(inner);
      qrInstance.value = new QRCode(inner, { width: 180, height: 180 });
    }
    qrInstance.value.clear();
    qrInstance.value.makeCode(deepLink.value);
  } catch (err) {
    console.error('Failed to render QR code', err);
  }
}

function resetOtpLater(): void {
  if (otpResetTimer) window.clearTimeout(otpResetTimer);
  otpResetTimer = window.setTimeout(() => {
    otpValue.value = '';
    otpMessage.value = t('pin.otp_expired_msg');
    otpStatus.value = 'warning';
  }, 3 * 60 * 1000);
}
async function requestOtp(): Promise<void> {
  if (otpBusy.value) return;
  if (editingHost.value) return;
  otpBusy.value = true;
  otpMessage.value = '';
  otpStatus.value = null;
  try {
    const response = await http.post(
      './api/otp',
      { passphrase: otpPassphrase.value, deviceName: otpDeviceName.value },
      { validateStatus: () => true },
    );
    const data = response.data as { status?: boolean; otp?: string; name?: string; ip?: string; message?: string } | undefined;
    if (!data || data.status !== true) {
      otpStatus.value = 'error';
      otpMessage.value = data?.message || t('_common.error');
      return;
    }
    otpValue.value = data.otp || '';
    hostName.value = data.name || '';
    otpStatus.value = 'success';
    otpMessage.value = t('pin.otp_success');

    const isLocalHost = typeof window !== 'undefined' && ['localhost', '127.0.0.1', '[::1]'].includes(window.location.hostname);

    if (!hostManuallySet.value) {
      if (typeof window !== 'undefined') {
        hostAddr.value = data.ip || (isLocalHost ? '127.0.0.1' : window.location.hostname);
        hostPort.value = window.location.port ? String(Math.max(parseInt(window.location.port, 10) - 1, 0)) : '47984';
      }
      saveHostCache();
    }

    await updateQrCode();
    resetOtpLater();

    if (!isLocalHost && deepLink.value) {
      setTimeout(() => {
        try {
          if (window.confirm(t('pin.otp_pair_now'))) window.open(deepLink.value, '_blank');
        } catch {
          /* ignore */
        }
      }, 0);
    }
  } catch (err) {
    console.error('OTP request failed', err);
    otpStatus.value = 'error';
    otpMessage.value = t('_common.error');
  } finally {
    otpBusy.value = false;
  }
}

// Client list state
const clients = ref<ClientRecord[]>([]);
const platform = ref('');
const unpairAllPressed = ref(false);
const unpairAllStatus = ref<boolean | null>(null);
const removing = reactive<Record<string, boolean>>({});
const showConfirmRemove = ref(false);
const pendingRemoveUuid = ref('');
const pendingRemoveName = ref('');
const showConfirmUnpairAll = ref(false);
const singleUnpairStatus = ref<'success' | null>(null);
const currentEditingId = ref<string | null>(null);

const permissionMapping = {
  input_controller: 0x00000100,
  input_touch: 0x00000200,
  input_pen: 0x00000400,
  input_mouse: 0x00000800,
  input_kbd: 0x00001000,
  _all_inputs: 0x00001f00,
  clipboard_set: 0x00010000,
  clipboard_read: 0x00020000,
  file_upload: 0x00040000,
  file_dwnload: 0x00080000,
  server_cmd: 0x00100000,
  _all_operations: 0x001f0000,
  list: 0x01000000,
  view: 0x02000000,
  launch: 0x04000000,
  _allow_view: 0x06000000,
  _all_actions: 0x07000000,
  _default: 0x03000000,
  _no: 0x00000000,
  _all: 0x071f1f00,
} as const;

type PermissionName = keyof typeof permissionMapping;

const permissionGroups = [
  {
    name: 'Action',
    label: 'Action',
    permissions: [
      { name: 'list', suppressed_by: ['view', 'launch'] as PermissionName[] },
      { name: 'view', suppressed_by: ['launch'] as PermissionName[] },
      { name: 'launch', suppressed_by: [] as PermissionName[] },
    ],
  },
  {
    name: 'Operation',
    label: 'Operation',
    permissions: [
      { name: 'clipboard_set', suppressed_by: [] as PermissionName[] },
      { name: 'clipboard_read', suppressed_by: [] as PermissionName[] },
      { name: 'server_cmd', suppressed_by: [] as PermissionName[] },
    ],
  },
  {
    name: 'Input',
    label: 'Input',
    permissions: [
      { name: 'input_controller', suppressed_by: [] as PermissionName[] },
      { name: 'input_touch', suppressed_by: [] as PermissionName[] },
      { name: 'input_pen', suppressed_by: [] as PermissionName[] },
      { name: 'input_mouse', suppressed_by: [] as PermissionName[] },
      { name: 'input_kbd', suppressed_by: [] as PermissionName[] },
    ],
  },
] as const;

type CommandSectionKey = 'doCmds' | 'undoCmds';
const commandSections = [
  { key: 'doCmds' as CommandSectionKey, label: 'pin.client_do_cmd', desc: 'pin.client_do_cmd_desc' },
  { key: 'undoCmds' as CommandSectionKey, label: 'pin.client_undo_cmd', desc: 'pin.client_undo_cmd_desc' },
] as const;

function normalizeCommands(list?: CommandEntry[]): CommandEntry[] {
  if (!Array.isArray(list)) return [];
  return list.map(({ cmd = '', elevated = 'false' }) => ({
    cmd,
    elevated: elevated === true ? 'true' : elevated === false ? 'false' : (elevated as string) || 'false',
  }));
}

function buildClientRecord(raw: RawClientInfo): ClientRecord {
  return {
    uuid: raw.uuid,
    name: raw.name || '',
    perm: Number.parseInt(String(raw.perm ?? 0), 10) || 0,
    display_mode: raw.display_mode || '',
    connected: Boolean(raw.connected),
    allow_client_commands: raw.allow_client_commands ?? 'true',
    enable_legacy_ordering: raw.enable_legacy_ordering ?? 'true',
    always_use_virtual_display: raw.always_use_virtual_display ?? 'false',
    do: normalizeCommands(raw.do),
    undo: normalizeCommands(raw.undo),
    editing: false,
  };
}

function clientDisplayPerm(client: ClientRecord): number {
  if (client.editing && client.edit) return client.edit.perm;
  return client.perm;
}

function permissionBadgeType(perm: number): 'default' | 'primary' | 'success' | 'info' | 'warning' | 'error' {
  return perm >= permissionMapping.launch ? 'error' : 'primary';
}

function isSuppressed(perm: number, permission: PermissionName, suppressed: PermissionName[]): boolean {
  return suppressed.some((item) => (perm & (permissionMapping[item] ?? 0)) !== 0);
}

function checkPermission(perm: number, permission: PermissionName): boolean {
  return (perm & (permissionMapping[permission] ?? 0)) !== 0;
}

async function refreshClients(): Promise<void> {
  if (!authStore.isAuthenticated) return;
  try {
    const r = await http.get<ClientsListResponse>('./api/clients/list', { validateStatus: () => true });
    const response = r.data || {};
    if (response.platform) platform.value = response.platform;
    if (response.status === true && Array.isArray(response.named_certs) && response.named_certs.length) {
      const mapped = response.named_certs.map(buildClientRecord);
      mapped.sort((a, b) => a.name.localeCompare(b.name));
      clients.value = mapped;
    } else {
      clients.value = [];
    }
  } catch (err) {
    console.error('Failed to refresh clients', err);
    clients.value = [];
  }
}

async function registerDevice(): Promise<void> {
  if (pairing.value) return;
  pairStatus.value = null;
  pairing.value = true;
  try {
    const trimmedName = deviceName.value.trim();
    const body = { pin: pin.value.trim(), name: trimmedName };
    const r = await http.post('./api/pin', body, { validateStatus: () => true });
    const ok =
      r &&
      r.status >= 200 &&
      r.status < 300 &&
      (r.data?.status === true || r.data?.status === 'true' || r.data?.status === 1);
    pairStatus.value = !!ok;
    if (ok) {
      const prevCount = clients.value.length;
      await refreshClients();
      const deadline = Date.now() + 5000;
      const target = trimmedName.toLowerCase();
      while (Date.now() < deadline) {
        const found = clients.value.some((c) => (c.name || '').toLowerCase() === target);
        if (found || clients.value.length > prevCount) break;
        await new Promise((res) => setTimeout(res, 400));
        await refreshClients();
      }
      pin.value = '';
      deviceName.value = '';
    }
  } catch (err) {
    console.error('PIN pairing failed', err);
    pairStatus.value = false;
  } finally {
    pairing.value = false;
    setTimeout(() => {
      pairStatus.value = null;
    }, 5000);
  }
}
function startEdit(client: ClientRecord): void {
  if (currentEditingId.value && currentEditingId.value !== client.uuid) {
    const existing = clients.value.find((c) => c.uuid === currentEditingId.value);
    if (existing) cancelEdit(existing);
  }
  client.edit = {
    name: client.name,
    perm: client.perm,
    displayMode: client.display_mode,
    allowClientCommands: client.allow_client_commands,
    enableLegacyOrdering: client.enable_legacy_ordering,
    alwaysUseVirtualDisplay: client.always_use_virtual_display,
    doCmds: normalizeCommands(client.do),
    undoCmds: normalizeCommands(client.undo),
  };
  client.editing = true;
  currentEditingId.value = client.uuid;
}

function cancelEdit(client: ClientRecord): void {
  client.editing = false;
  client.edit = undefined;
  if (currentEditingId.value === client.uuid) currentEditingId.value = null;
}

function togglePermission(client: ClientRecord, permission: PermissionName): void {
  if (!client.edit) return;
  client.edit.perm ^= permissionMapping[permission];
}

function permissionButtonType(perm: number, permission: PermissionName, suppressed: PermissionName[]):
  | 'default'
  | 'primary'
  | 'success'
  | 'info'
  | 'warning'
  | 'error' {
  if (isSuppressed(perm, permission, suppressed)) return 'default';
  return checkPermission(perm, permission) ? 'primary' : 'default';
}

function addCommand(client: ClientRecord, key: CommandSectionKey, index: number): void {
  if (!client.edit) return;
  const target = client.edit[key];
  const entry: CommandEntry = { cmd: '', elevated: 'false' };
  if (index < 0 || index >= target.length - 1) target.push(entry);
  else target.splice(index + 1, 0, entry);
}

function removeCommand(client: ClientRecord, key: CommandSectionKey, index: number): void {
  if (!client.edit) return;
  client.edit[key].splice(index, 1);
}

function permToStr(perm: number): string {
  const segments = [
    (perm >> 24) & 0xff,
    (perm >> 16) & 0xff,
    (perm >> 8) & 0xff,
  ];
  return segments.map((seg) => seg.toString(16).toUpperCase().padStart(2, '0')).join(' ');
}

async function saveClient(client: ClientRecord): Promise<void> {
  if (!client.edit) return;
  const { name, displayMode, allowClientCommands, enableLegacyOrdering, alwaysUseVirtualDisplay } = client.edit;
  const trimmedDisplay = displayMode.trim();
  if (trimmedDisplay && !/^\d+x\d+x\d+(\.\d+)?$/.test(trimmedDisplay)) {
    message.error(t('pin.display_mode_override_error'));
    return;
  }

  const payload = {
    uuid: client.uuid,
    name: name.trim(),
    display_mode: trimmedDisplay,
    allow_client_commands: allowClientCommands,
    enable_legacy_ordering: enableLegacyOrdering,
    always_use_virtual_display: alwaysUseVirtualDisplay,
    perm: client.edit.perm & permissionMapping._all,
    do: client.edit.doCmds
      .map(({ cmd, elevated }) => ({ cmd: cmd.trim(), elevated }))
      .filter((entry) => entry.cmd.length > 0),
    undo: client.edit.undoCmds
      .map(({ cmd, elevated }) => ({ cmd: cmd.trim(), elevated }))
      .filter((entry) => entry.cmd.length > 0),
  };

  try {
    const resp = await http.post('./api/clients/update', payload, { validateStatus: () => true });
    if (resp.data?.status) {
      message.success(t('_common.success'));
      cancelEdit(client);
      await refreshClients();
    } else {
      const msg = resp.data?.message
        ? t('pin.save_client_error') + resp.data.message
        : t('pin.save_client_error');
      message.error(msg);
    }
  } catch (err) {
    console.error('Failed saving client', err);
    message.error(t('pin.save_client_error'));
  }
}

async function disconnectClient(uuid: string): Promise<void> {
  try {
    await http.post('./api/clients/disconnect', { uuid }, { validateStatus: () => true });
  } catch (err) {
    console.error('Failed to disconnect client', err);
  } finally {
    setTimeout(() => {
      void refreshClients();
    }, 1000);
  }
}

function askConfirmUnpair(uuid: string): void {
  pendingRemoveUuid.value = uuid;
  const found = clients.value.find((c) => c.uuid === uuid);
  pendingRemoveName.value = found?.name || '';
  showConfirmRemove.value = true;
}

async function confirmRemove(): Promise<void> {
  const uuid = pendingRemoveUuid.value;
  showConfirmRemove.value = false;
  pendingRemoveUuid.value = '';
  pendingRemoveName.value = '';
  if (!uuid) return;
  await unpairSingle(uuid);
}

async function unpairSingle(uuid: string): Promise<void> {
  if (removing[uuid]) return;
  removing[uuid] = true;
  try {
    await http.post('./api/clients/unpair', { uuid }, { validateStatus: () => true });
    singleUnpairStatus.value = 'success';
  } catch (err) {
    console.error('Failed to unpair client', err);
    singleUnpairStatus.value = null;
    message.error(t('_common.error'));
  } finally {
    delete removing[uuid];
    setTimeout(() => {
      singleUnpairStatus.value = null;
    }, 5000);
    void refreshClients();
  }
}

function askConfirmUnpairAll(): void {
  showConfirmUnpairAll.value = true;
}

async function confirmUnpairAll(): Promise<void> {
  showConfirmUnpairAll.value = false;
  await unpairAll();
}

async function unpairAll(): Promise<void> {
  unpairAllPressed.value = true;
  try {
    const r = await http.post('./api/clients/unpair-all', {}, { validateStatus: () => true });
    unpairAllStatus.value = r.data?.status === true;
  } catch (err) {
    console.error('Failed to unpair all', err);
    unpairAllStatus.value = false;
  } finally {
    unpairAllPressed.value = false;
    setTimeout(() => {
      unpairAllStatus.value = null;
    }, 5000);
    void refreshClients();
  }
}

async function init(): Promise<void> {
  await authStore.waitForAuthentication();
  loadHostCache();
  await refreshClients();
}

onMounted(() => {
  void init();
});
</script>

<style scoped></style>
