<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../../PlatformLayout.vue'
import Checkbox from "../../../Checkbox.vue";
import { useConfigStore } from '../../../stores/config.js'

// Use centralized store for config and platform
const store = useConfigStore()
const config = store.config

const REFRESH_RATE_ONLY = "refresh_rate_only"
const RESOLUTION_ONLY = "resolution_only"
const MIXED = "mixed"

function canBeRemapped() {
  return (config.value.dd_resolution_option === "auto" || config.value.dd_refresh_rate_option === "auto")
    && config.value.dd_configuration_option !== "disabled";
}

function getRemappingType() {
  // Assuming here that at least one setting is set to "auto" if other is not
  if (config.value.dd_resolution_option !== "auto") {
    return REFRESH_RATE_ONLY;
  }
  if (config.value.dd_refresh_rate_option !== "auto") {
    return RESOLUTION_ONLY;
  }
  return MIXED;
}

function addRemappingEntry() {
  const type = getRemappingType();
  let template = {};

  if (type !== RESOLUTION_ONLY) {
    template["requested_fps"] = "";
    template["final_refresh_rate"] = "";
  }

  if (type !== REFRESH_RATE_ONLY) {
    template["requested_resolution"] = "";
    template["final_resolution"] = "";
  }

  config.value.dd_mode_remapping[type].push(template);
}
</script>

<template>
  <PlatformLayout>
    <template #windows>
      <div class="mb-6">
        <div class="rounded-md overflow-hidden border border-black/5 dark:border-white/10">
          <div class="bg-solar-surface/40 dark:bg-lunar-surface/30 px-4 py-3">
            <h3 class="text-sm font-medium">{{ $t('config.dd_options_header') }}</h3>
          </div>
          <div class="p-4 space-y-4">

            <!-- Configuration option -->
            <div>
              <label for="dd_configuration_option" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">
                {{ $t('config.dd_config_label') }}
              </label>
              <select id="dd_configuration_option" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40" v-model="config.dd_configuration_option">
                <option value="disabled">{{ $t('_common.disabled_def') }}</option>
                <option value="verify_only">{{ $t('config.dd_config_verify_only') }}</option>
                <option value="ensure_active">{{ $t('config.dd_config_ensure_active') }}</option>
                <option value="ensure_primary">{{ $t('config.dd_config_ensure_primary') }}</option>
                <option value="ensure_only_display">{{ $t('config.dd_config_ensure_only_display') }}</option>
              </select>
            </div>

            <!-- Resolution option -->
            <div v-if="config.dd_configuration_option !== 'disabled'">
              <label for="dd_resolution_option" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">
                {{ $t('config.dd_resolution_option') }}
              </label>
              <select id="dd_resolution_option" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40" v-model="config.dd_resolution_option">
                <option value="disabled">{{ $t('config.dd_resolution_option_disabled') }}</option>
                <option value="auto">{{ $t('config.dd_resolution_option_auto') }}</option>
                <option value="manual">{{ $t('config.dd_resolution_option_manual') }}</option>
              </select>
              <p class="text-[11px] opacity-60 mt-1" v-if="config.dd_resolution_option === 'auto' || config.dd_resolution_option === 'manual'">
                {{ $t('config.dd_resolution_option_ogs_desc') }}
              </p>

              <div class="mt-2 pl-4" v-if="config.dd_resolution_option === 'manual'">
                <p class="text-[11px] opacity-60">{{ $t('config.dd_resolution_option_manual_desc') }}</p>
                <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 monospace" id="dd_manual_resolution" placeholder="2560x1440"
                       v-model="config.dd_manual_resolution" />
              </div>
            </div>

            <!-- Refresh rate option -->
            <div v-if="config.dd_configuration_option !== 'disabled'">
              <label for="dd_refresh_rate_option" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">
                {{ $t('config.dd_refresh_rate_option') }}
              </label>
              <select id="dd_refresh_rate_option" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40" v-model="config.dd_refresh_rate_option">
                <option value="disabled">{{ $t('config.dd_refresh_rate_option_disabled') }}</option>
                <option value="auto">{{ $t('config.dd_refresh_rate_option_auto') }}</option>
                <option value="manual">{{ $t('config.dd_refresh_rate_option_manual') }}</option>
              </select>

              <div class="mt-2 pl-4" v-if="config.dd_refresh_rate_option === 'manual'">
                <p class="text-[11px] opacity-60">{{ $t('config.dd_refresh_rate_option_manual_desc') }}</p>
                <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 monospace" id="dd_manual_refresh_rate" placeholder="59.9558"
                       v-model="config.dd_manual_refresh_rate" />
              </div>
            </div>

            <!-- HDR option -->
            <div v-if="config.dd_configuration_option !== 'disabled'">
              <label for="dd_hdr_option" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">
                {{ $t('config.dd_hdr_option') }}
              </label>
              <select id="dd_hdr_option" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40 mb-2" v-model="config.dd_hdr_option">
                <option value="disabled">{{ $t('config.dd_hdr_option_disabled') }}</option>
                <option value="auto">{{ $t('config.dd_hdr_option_auto') }}</option>
              </select>

              <label for="dd_wa_hdr_toggle_delay" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">
                {{ $t('config.dd_wa_hdr_toggle_delay') }}
              </label>
              <input type="number" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15" id="dd_wa_hdr_toggle_delay" placeholder="0" min="0" max="3000"
                     v-model="config.dd_wa_hdr_toggle_delay" />
              <p class="text-[11px] opacity-60 mt-1">
                {{ $t('config.dd_wa_hdr_toggle_delay_desc_1') }}<br>
                {{ $t('config.dd_wa_hdr_toggle_delay_desc_2') }}<br>
                {{ $t('config.dd_wa_hdr_toggle_delay_desc_3') }}
              </p>
            </div>

            <!-- Config revert delay -->
            <div v-if="config.dd_configuration_option !== 'disabled'">
              <label for="dd_config_revert_delay" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">
                {{ $t('config.dd_config_revert_delay') }}
              </label>
              <input type="number" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15" id="dd_config_revert_delay" placeholder="3000" min="0"
                     v-model="config.dd_config_revert_delay" />
              <p class="text-[11px] opacity-60 mt-1">{{ $t('config.dd_config_revert_delay_desc') }}</p>
            </div>

            <!-- Config revert on disconnect -->
            <div>
              <Checkbox id="dd_config_revert_on_disconnect"
                locale-prefix="config"
                v-model="config.dd_config_revert_on_disconnect"
                default="false"
              ></Checkbox>
            </div>

            <!-- Display mode remapping -->
            <div v-if="canBeRemapped()">
              <label for="dd_mode_remapping" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">
                {{ $t('config.dd_mode_remapping') }}
              </label>
              <p class="text-[11px] opacity-60">
                {{ $t('config.dd_mode_remapping_desc_1') }}<br>
                {{ $t('config.dd_mode_remapping_desc_2') }}<br>
                {{ $t('config.dd_mode_remapping_desc_3') }}<br>
                {{ $t(getRemappingType() === MIXED ? 'config.dd_mode_remapping_desc_4_final_values_mixed' : 'config.dd_mode_remapping_desc_4_final_values_non_mixed') }}<br>
                <template v-if="getRemappingType() === MIXED">
                  {{ $t('config.dd_mode_remapping_desc_5_sops_mixed_only') }}<br>
                </template>
                <template v-if="getRemappingType() === RESOLUTION_ONLY">
                  {{ $t('config.dd_mode_remapping_desc_5_sops_resolution_only') }}<br>
                </template>
              </p>

              <div v-if="config.dd_mode_remapping[getRemappingType()].length > 0" class="space-y-2">
                <div v-for="(value, idx) in config.dd_mode_remapping[getRemappingType()]" :key="idx" class="grid grid-cols-12 gap-2 items-center">
                  <div v-if="getRemappingType() !== REFRESH_RATE_ONLY" class="col-span-3">
                    <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 monospace" v-model="value.requested_resolution" :placeholder="'1920x1080'" />
                  </div>
                  <div v-if="getRemappingType() !== RESOLUTION_ONLY" class="col-span-2">
                    <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 monospace" v-model="value.requested_fps" :placeholder="'60'" />
                  </div>
                  <div v-if="getRemappingType() !== REFRESH_RATE_ONLY" class="col-span-3">
                    <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 monospace" v-model="value.final_resolution" :placeholder="'2560x1440'" />
                  </div>
                  <div v-if="getRemappingType() !== RESOLUTION_ONLY" class="col-span-2">
                    <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 monospace" v-model="value.final_refresh_rate" :placeholder="'119.95'" />
                  </div>
                  <div class="col-span-2 text-right">
                    <button class="inline-flex items-center justify-center px-3 py-2 rounded-md bg-red-600 text-white text-sm hover:bg-red-700" @click="config.dd_mode_remapping[getRemappingType()].splice(idx, 1)">
                      <i class="fas fa-trash"></i>
                    </button>
                  </div>
                </div>
              </div>
              <div class="mt-2">
                <button class="inline-flex items-center gap-2 px-3 py-2 rounded-md bg-green-600 text-white text-sm hover:bg-green-700" @click="addRemappingEntry()">
                  &plus; {{ $t('config.dd_mode_remapping_add') }}
                </button>
              </div>
            </div>
          </div>
        </div>
      </div>
    </template>
    <template #linux>
    </template>
    <template #macos>
    </template>
  </PlatformLayout>
</template>
