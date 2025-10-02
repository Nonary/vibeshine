<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue';
import { storeToRefs } from 'pinia';
import { useI18n } from 'vue-i18n';
import { NInput } from 'naive-ui';
import Checkbox from '@/Checkbox.vue';
import PlatformLayout from '@/PlatformLayout.vue';
import AdapterNameSelector from '@/configs/tabs/audiovideo/AdapterNameSelector.vue';
import DisplayOutputSelector from '@/configs/tabs/audiovideo/DisplayOutputSelector.vue';
import DisplayDeviceOptions from '@/configs/tabs/audiovideo/DisplayDeviceOptions.vue';
import DisplayModesSettings from '@/configs/tabs/audiovideo/DisplayModesSettings.vue';
import FrameLimiterStep from '@/configs/tabs/audiovideo/FrameLimiterStep.vue';
import { $tp } from '@/platform-i18n';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);
const { t } = useI18n();

const platform = computed(
  () => (metadata.value?.platform || config.value?.platform || '').toString().toLowerCase(),
);
const ddConfigDisabled = computed(() => (config.value as any)?.dd_configuration_option === 'disabled');
const frameLimiterStepLabel = computed(() =>
  ddConfigDisabled.value ? t('config.dd_step_3') : t('config.dd_step_4'),
);

const fallbackInput = ref('');
const fallbackError = ref(false);
const fallbackCache = ref('');
const fallbackPattern = /^\d+x\d+x\d+(?:\.\d+)?$/i;

onMounted(() => {
  const current = (config.value as any)?.fallback_mode;
  fallbackInput.value = typeof current === 'string' ? current : '';
  fallbackCache.value = fallbackInput.value;
});

watch(
  () => (config.value as any)?.fallback_mode,
  (val) => {
    const str = typeof val === 'string' ? val : '';
    fallbackCache.value = str;
    if (!fallbackError.value) {
      fallbackInput.value = str;
    }
  },
);

function handleFallbackChange(val: string) {
  fallbackInput.value = val;
  const trimmed = val.trim();
  if (!trimmed) {
    fallbackCache.value = '';
    fallbackError.value = false;
    if (config.value) (config.value as any).fallback_mode = '';
    return;
  }
  if (fallbackPattern.test(trimmed)) {
    fallbackCache.value = trimmed;
    fallbackError.value = false;
    if (config.value) (config.value as any).fallback_mode = trimmed;
  } else {
    fallbackError.value = true;
  }
}

function handleFallbackBlur() {
  if (!fallbackError.value) return;
  fallbackInput.value = fallbackCache.value;
  fallbackError.value = false;
}
</script>

<template>
  <div class="config-page space-y-6">
    <section class="space-y-4">
      <div>
        <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
        <n-input
          id="audio_sink"
          v-model:value="config.audio_sink"
          type="text"
          :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
        />
        <div class="text-[11px] opacity-60 mt-1 space-y-2">
          <p>{{ $tp('config.audio_sink_desc') }}</p>
          <PlatformLayout>
            <template #windows>
              <pre class="font-mono text-[11px] bg-dark/5 dark:bg-light/10 rounded px-2 py-1">tools\\audio-info.exe</pre>
            </template>
            <template #linux>
              <pre class="font-mono text-[11px] bg-dark/5 dark:bg-light/10 rounded px-2 py-1">pacmd list-sinks | grep "name:"</pre>
              <pre class="font-mono text-[11px] bg-dark/5 dark:bg-light/10 rounded px-2 py-1">pactl info | grep Source</pre>
            </template>
            <template #macos>
              <div class="space-y-1">
                <a href="https://github.com/mattingalls/Soundflower" target="_blank" rel="noopener">Soundflower</a>
                <br />
                <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank" rel="noopener">BlackHole</a>
              </div>
            </template>
          </PlatformLayout>
        </div>
      </div>

      <PlatformLayout>
        <template #windows>
          <div class="space-y-4">
            <div>
              <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
              <n-input
                id="virtual_sink"
                v-model:value="config.virtual_sink"
                type="text"
                :placeholder="$t('config.virtual_sink_placeholder')"
              />
              <p class="text-[11px] opacity-60 mt-1">{{ $t('config.virtual_sink_desc') }}</p>
            </div>
            <Checkbox
              id="install_steam_audio_drivers"
              v-model="config.install_steam_audio_drivers"
              locale-prefix="config"
              default="true"
            />
            <Checkbox
              id="keep_sink_default"
              v-model="config.keep_sink_default"
              locale-prefix="config"
              default="true"
            />
            <Checkbox
              id="auto_capture_sink"
              v-model="config.auto_capture_sink"
              locale-prefix="config"
              default="true"
            />
          </div>
        </template>
      </PlatformLayout>

      <Checkbox id="stream_audio" v-model="config.stream_audio" locale-prefix="config" default="true" />
    </section>

    <AdapterNameSelector />

    <section class="space-y-6">
      <div class="rounded-md overflow-hidden border border-dark/10 dark:border-light/10">
        <div class="bg-surface/40 px-4 py-3">
          <h3 class="text-sm font-medium">{{ $t('config.dd_display_setup_title') }}</h3>
          <p class="text-[11px] opacity-60 mt-1">{{ $t('config.dd_display_setup_intro') }}</p>
        </div>
        <div class="p-4 space-y-4">
          <fieldset class="border border-dark/35 dark:border-light/25 rounded-xl p-4">
            <legend class="px-2 text-sm font-medium">
              {{ $t('config.dd_step_1') }} · {{ $t('config.dd_choose_display') }}
            </legend>
            <DisplayOutputSelector />
          </fieldset>

          <DisplayDeviceOptions section="pre" />

          <DisplayDeviceOptions section="options" />

          <FrameLimiterStep :step-label="frameLimiterStepLabel" />
        </div>
      </div>
    </section>

    <DisplayModesSettings />

    <section class="space-y-3">
      <div>
        <label for="fallback_mode" class="form-label">{{ $t('config.fallback_mode') }}</label>
        <n-input
          id="fallback_mode"
          v-model:value="fallbackInput"
          type="text"
          placeholder="1920x1080x60"
          :status="fallbackError ? 'error' : undefined"
          @update:value="handleFallbackChange"
          @blur="handleFallbackBlur"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.fallback_mode_desc') }}</p>
        <p v-if="fallbackError" class="text-[11px] text-danger mt-1">
          {{ $t('config.fallback_mode_error') }}
        </p>
      </div>
      <PlatformLayout>
        <template #windows>
          <Checkbox
            id="headless_mode"
            v-model="config.headless_mode"
            locale-prefix="config"
            default="false"
          />
          <Checkbox
            id="double_refreshrate"
            v-model="config.double_refreshrate"
            locale-prefix="config"
            default="false"
          />
          <Checkbox
            id="isolated_virtual_display_option"
            v-model="config.isolated_virtual_display_option"
            locale-prefix="config"
            default="false"
          />
        </template>
      </PlatformLayout>
    </section>
  </div>
</template>
