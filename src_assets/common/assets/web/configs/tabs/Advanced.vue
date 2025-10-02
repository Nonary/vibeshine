<script setup lang="ts">
import { NSelect, NInputNumber } from 'naive-ui';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;

const hevcModeOptions = [0, 1, 2, 3].map((v) => ({ labelKey: `config.hevc_mode_${v}`, value: v }));
const av1ModeOptions = [0, 1, 2, 3].map((v) => ({ labelKey: `config.av1_mode_${v}`, value: v }));
</script>

<template>
  <div class="config-page space-y-6">
    <section class="space-y-4">
      <div>
        <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
        <n-input-number id="fec_percentage" v-model:value="config.fec_percentage" placeholder="20" />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.fec_percentage_desc') }}</p>
      </div>

      <div>
        <label for="qp" class="form-label">{{ $t('config.qp') }}</label>
        <n-input-number id="qp" v-model:value="config.qp" placeholder="28" />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.qp_desc') }}</p>
      </div>

      <div>
        <label for="min_threads" class="form-label">{{ $t('config.min_threads') }}</label>
        <n-input-number id="min_threads" v-model:value="config.min_threads" :min="1" placeholder="2" />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.min_threads_desc') }}</p>
      </div>
    </section>

    <section class="space-y-3">
      <Checkbox id="limit_framerate" v-model="config.limit_framerate" locale-prefix="config" default="true" />
      <Checkbox
        id="envvar_compatibility_mode"
        v-model="config.envvar_compatibility_mode"
        locale-prefix="config"
        default="false"
      />
      <Checkbox
        id="legacy_ordering"
        v-model="config.legacy_ordering"
        locale-prefix="config"
        default="false"
      />
      <Checkbox
        id="ignore_encoder_probe_failure"
        v-model="config.ignore_encoder_probe_failure"
        locale-prefix="config"
        default="false"
      />
    </section>

    <section class="space-y-4">
      <div>
        <label for="hevc_mode" class="form-label">{{ $t('config.hevc_mode') }}</label>
        <n-select
          id="hevc_mode"
          v-model:value="config.hevc_mode"
          :options="hevcModeOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
          :data-search-options="hevcModeOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.hevc_mode_desc') }}</p>
      </div>

      <div>
        <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
        <n-select
          id="av1_mode"
          v-model:value="config.av1_mode"
          :options="av1ModeOptions.map((o) => ({ label: $t(o.labelKey), value: o.value }))"
          :data-search-options="av1ModeOptions.map((o) => `${$t(o.labelKey)}::${o.value}`).join('|')"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.av1_mode_desc') }}</p>
      </div>
    </section>
  </div>
</template>
