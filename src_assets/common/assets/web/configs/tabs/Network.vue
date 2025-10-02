<script setup lang="ts">
import { computed } from 'vue';
import { NSelect, NInput, NInputNumber } from 'naive-ui';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
const defaultMoonlightPort = 47989;

const effectivePort = computed(() => Number(config.value?.port ?? defaultMoonlightPort));

const addressFamilyOptions = [
  { label: 'config.address_family_ipv4', value: 'ipv4' },
  { label: 'config.address_family_both', value: 'both' },
];

const originUiOptions = [
  { label: 'config.origin_web_ui_allowed_pc', value: 'pc' },
  { label: 'config.origin_web_ui_allowed_lan', value: 'lan' },
  { label: 'config.origin_web_ui_allowed_wan', value: 'wan' },
];

const encryptionModeOptionsLan = [
  { label: '_common.disabled_def', value: 0 },
  { label: 'config.lan_encryption_mode_1', value: 1 },
  { label: 'config.lan_encryption_mode_2', value: 2 },
];

const encryptionModeOptionsWan = [
  { label: '_common.disabled', value: 0 },
  { label: 'config.wan_encryption_mode_1', value: 1 },
  { label: 'config.wan_encryption_mode_2', value: 2 },
];

const portRows = computed(() => {
  const base = effectivePort.value;
  return [
    { protocol: 'config.port_tcp', port: base - 5, note: '' },
    {
      protocol: 'config.port_tcp',
      port: base,
      note: base !== defaultMoonlightPort ? 'config.port_http_port_note' : '',
    },
    { protocol: 'config.port_tcp', port: base + 1, note: 'config.port_web_ui' },
    { protocol: 'config.port_tcp', port: base + 21, note: '' },
    { protocol: 'config.port_udp', port: `${base + 9} – ${base + 11}`, note: '' },
  ];
});
</script>

<template>
  <div class="config-page space-y-6">
    <section class="space-y-4">
      <Checkbox id="upnp" v-model="config.upnp" locale-prefix="config" default="false" />

      <div>
        <label for="address_family" class="form-label">{{ $t('config.address_family') }}</label>
        <n-select
          id="address_family"
          v-model:value="config.address_family"
          :options="addressFamilyOptions.map((o) => ({ label: $t(o.label), value: o.value }))"
          :data-search-options="addressFamilyOptions.map((o) => `${$t(o.label)}::${o.value}`).join('|')"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.address_family_desc') }}</p>
      </div>

      <div>
        <label for="port" class="form-label">{{ $t('config.port') }}</label>
        <n-input-number
          id="port"
          v-model:value="config.port"
          :min="1029"
          :max="65514"
          :placeholder="String(defaultMoonlightPort)"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.port_desc') }}</p>

        <div
          v-if="+effectivePort - 5 < 1024"
          class="mt-2 alert alert-danger p-2 flex items-start gap-2 rounded-md"
        >
          <i class="fa-solid fa-triangle-exclamation mt-[2px]" />
          <span class="text-sm">{{ $t('config.port_alert_1') }}</span>
        </div>
        <div
          v-if="+effectivePort + 21 > 65535"
          class="mt-2 alert alert-danger p-2 flex items-start gap-2 rounded-md"
        >
          <i class="fa-solid fa-triangle-exclamation mt-[2px]" />
          <span class="text-sm">{{ $t('config.port_alert_2') }}</span>
        </div>

        <div class="mt-4 grid grid-cols-12 gap-2 text-[11px]">
          <div class="col-span-4 font-semibold">{{ $t('config.port_protocol') }}</div>
          <div class="col-span-4 font-semibold">{{ $t('config.port_port') }}</div>
          <div class="col-span-4 font-semibold">{{ $t('config.port_note') }}</div>
          <template v-for="(row, idx) in portRows" :key="idx">
            <div class="col-span-4">{{ $t(row.protocol) }}</div>
            <div class="col-span-4">{{ row.port }}</div>
            <div class="col-span-4">
              <div v-if="row.note" class="alert alert-info p-2 rounded-md text-xs">
                {{ $t(row.note) }}
              </div>
            </div>
          </template>
        </div>

        <div
          v-if="config.origin_web_ui_allowed === 'wan'"
          class="mt-3 alert alert-warning p-2 flex items-start gap-2 rounded-md"
        >
          <i class="fa-solid fa-triangle-exclamation mt-[2px]" />
          <span class="text-sm">{{ $t('config.port_warning') }}</span>
        </div>
      </div>
    </section>

    <section class="space-y-4">
      <div>
        <label for="origin_web_ui_allowed" class="form-label">{{
          $t('config.origin_web_ui_allowed')
        }}</label>
        <n-select
          id="origin_web_ui_allowed"
          v-model:value="config.origin_web_ui_allowed"
          :options="originUiOptions.map((o) => ({ label: $t(o.label), value: o.value }))"
          :data-search-options="originUiOptions.map((o) => `${$t(o.label)}::${o.value}`).join('|')"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.origin_web_ui_allowed_desc') }}</p>
      </div>

      <div>
        <label for="external_ip" class="form-label">{{ $t('config.external_ip') }}</label>
        <n-input id="external_ip" v-model:value="config.external_ip" type="text" placeholder="123.456.78.9" />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.external_ip_desc') }}</p>
      </div>

      <div>
        <label for="lan_encryption_mode" class="form-label">{{ $t('config.lan_encryption_mode') }}</label>
        <n-select
          id="lan_encryption_mode"
          v-model:value="config.lan_encryption_mode"
          :options="encryptionModeOptionsLan.map((o) => ({ label: $t(o.label), value: o.value }))"
          :data-search-options="encryptionModeOptionsLan.map((o) => `${$t(o.label)}::${o.value}`).join('|')"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.lan_encryption_mode_desc') }}</p>
      </div>

      <div>
        <label for="wan_encryption_mode" class="form-label">{{ $t('config.wan_encryption_mode') }}</label>
        <n-select
          id="wan_encryption_mode"
          v-model:value="config.wan_encryption_mode"
          :options="encryptionModeOptionsWan.map((o) => ({ label: $t(o.label), value: o.value }))"
          :data-search-options="encryptionModeOptionsWan.map((o) => `${$t(o.label)}::${o.value}`).join('|')"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.wan_encryption_mode_desc') }}</p>
      </div>

      <div>
        <label for="ping_timeout" class="form-label">{{ $t('config.ping_timeout') }}</label>
        <n-input-number
          id="ping_timeout"
          v-model:value="config.ping_timeout"
          :min="0"
          :step="100"
          placeholder="10000"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.ping_timeout_desc') }}</p>
      </div>
    </section>
  </div>
</template>
