<script setup>
import { ref } from 'vue';
import PlatformLayout from '@/PlatformLayout.vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';
import { computed } from 'vue';

const store = useConfigStore();
const config = store.config;
const platform = computed(() => config.value?.platform || '');
</script>

<template>
  <div id="input" class="config-page">
    <!-- Enable Gamepad Input -->
    <Checkbox
      id="controller"
      v-model="config.controller"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- Emulated Gamepad Type -->
    <div v-if="config.controller === 'enabled' && platform !== 'macos'" class="mb-6">
      <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
      <select id="gamepad" v-model="config.gamepad" class="form-control">
        <option value="auto">
          {{ $t('_common.auto') }}
        </option>

        <PlatformLayout>
          <template #linux>
            <option value="ds5">
              {{ $t('config.gamepad_ds5') }}
            </option>
            <option value="switch">
              {{ $t('config.gamepad_switch') }}
            </option>
            <option value="xone">
              {{ $t('config.gamepad_xone') }}
            </option>
          </template>

          <template #windows>
            <option value="ds4">
              {{ $t('config.gamepad_ds4') }}
            </option>
            <option value="x360">
              {{ $t('config.gamepad_x360') }}
            </option>
          </template>
        </PlatformLayout>
      </select>
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.gamepad_desc') }}
      </p>
    </div>

    <!-- Additional options based on gamepad type -->
    <template v-if="config.controller === 'enabled'">
      <template
        v-if="config.gamepad === 'ds4' || (config.gamepad === 'auto' && platform === 'windows')"
      >
        <div class="mb-4">
          <div
            class="px-3 py-2 rounded-md bg-white/80 dark:bg-surface/70 border border-black/5 dark:border-white/10"
          >
            <div class="font-medium mb-2">
              {{
                $t(config.gamepad === 'ds4' ? 'config.gamepad_ds4_manual' : 'config.gamepad_auto')
              }}
            </div>
            <div class="space-y-2">
              <template v-if="config.gamepad === 'auto'">
                <Checkbox
                  id="motion_as_ds4"
                  v-model="config.motion_as_ds4"
                  class="mb-3"
                  locale-prefix="config"
                  default="true"
                />
                <Checkbox
                  id="touchpad_as_ds4"
                  v-model="config.touchpad_as_ds4"
                  class="mb-3"
                  locale-prefix="config"
                  default="true"
                />
              </template>
              <template v-if="config.gamepad === 'ds4'">
                <Checkbox
                  id="ds4_back_as_touchpad_click"
                  v-model="config.ds4_back_as_touchpad_click"
                  class="mb-3"
                  locale-prefix="config"
                  default="true"
                />
              </template>
            </div>
          </div>
        </div>
      </template>
    </template>

    <!-- Home/Guide Button Emulation Timeout -->
    <div v-if="config.controller === 'enabled'" class="mb-4">
      <label for="back_button_timeout" class="form-label">{{
        $t('config.back_button_timeout')
      }}</label>
      <input
        id="back_button_timeout"
        v-model="config.back_button_timeout"
        type="text"
        class="form-control"
        placeholder="-1"
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.back_button_timeout_desc') }}
      </p>
    </div>

    <!-- Enable Keyboard Input -->
    <hr />
    <Checkbox
      id="keyboard"
      v-model="config.keyboard"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- Key Repeat Delay-->
    <div v-if="config.keyboard === 'enabled' && platform === 'windows'" class="mb-4">
      <label for="key_repeat_delay" class="form-label">{{ $t('config.key_repeat_delay') }}</label>
      <input
        id="key_repeat_delay"
        v-model="config.key_repeat_delay"
        type="text"
        class="form-control"
        placeholder="500"
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.key_repeat_delay_desc') }}
      </p>
    </div>

    <!-- Key Repeat Frequency-->
    <div v-if="config.keyboard === 'enabled' && platform === 'windows'" class="mb-4">
      <label for="key_repeat_frequency" class="form-label">{{
        $t('config.key_repeat_frequency')
      }}</label>
      <input
        id="key_repeat_frequency"
        v-model="config.key_repeat_frequency"
        type="text"
        class="form-control"
        placeholder="24.9"
      />
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.key_repeat_frequency_desc') }}
      </p>
    </div>

    <!-- Always send scancodes -->
    <Checkbox
      v-if="config.keyboard === 'enabled' && platform === 'windows'"
      id="always_send_scancodes"
      v-model="config.always_send_scancodes"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- Mapping Key AltRight to Key Windows -->
    <Checkbox
      v-if="config.keyboard === 'enabled'"
      id="key_rightalt_to_key_win"
      v-model="config.key_rightalt_to_key_win"
      class="mb-3"
      locale-prefix="config"
      default="false"
    />

    <!-- Enable Mouse Input -->
    <hr />
    <Checkbox
      id="mouse"
      v-model="config.mouse"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- High resolution scrolling support -->
    <Checkbox
      v-if="config.mouse === 'enabled'"
      id="high_resolution_scrolling"
      v-model="config.high_resolution_scrolling"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />

    <!-- Native pen/touch support -->
    <Checkbox
      v-if="config.mouse === 'enabled'"
      id="native_pen_touch"
      v-model="config.native_pen_touch"
      class="mb-3"
      locale-prefix="config"
      default="true"
    />
  </div>
</template>

<style scoped></style>
