<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'
import Checkbox from "../../Checkbox.vue";
import { useConfigStore } from '../../stores/config.js'
import { computed } from 'vue'

const store = useConfigStore()
const config = store.config
const platform = computed(() => config.value?.platform || '')
</script>

<template>
  <div id="input" class="config-page">
    <!-- Enable Gamepad Input -->
    <Checkbox class="mb-3"
              id="controller"
              locale-prefix="config"
              v-model="config.controller"
              default="true"
    ></Checkbox>

    <!-- Emulated Gamepad Type -->
    <div class="mb-6" v-if="config.controller === 'enabled' && platform !== 'macos'">
      <label for="gamepad" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.gamepad') }}</label>
      <select id="gamepad" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40" v-model="config.gamepad">
        <option value="auto">{{ $t('_common.auto') }}</option>

  <PlatformLayout>
          <template #linux>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
          </template>
          
          <template #windows>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
          </template>
        </PlatformLayout>
      </select>
  <p class="text-[11px] opacity-60 mt-1">{{ $t('config.gamepad_desc') }}</p>
    </div>

    <!-- Additional options based on gamepad type -->
    <template v-if="config.controller === 'enabled'">
      <template v-if="config.gamepad === 'ds4' || (config.gamepad === 'auto' && platform === 'windows')">
        <div class="mb-4">
          <div class="px-3 py-2 rounded-md bg-white/80 dark:bg-lunar-surface/70 border border-black/5 dark:border-white/10">
            <div class="font-medium mb-2">{{ $t(config.gamepad === 'ds4' ? 'config.gamepad_ds4_manual' : 'config.gamepad_auto') }}</div>
            <div class="space-y-2">
              <template v-if="config.gamepad === 'auto'">
                <Checkbox class="mb-3"
                          id="motion_as_ds4"
                          locale-prefix="config"
                          v-model="config.motion_as_ds4"
                          default="true"
                ></Checkbox>
                <Checkbox class="mb-3"
                          id="touchpad_as_ds4"
                          locale-prefix="config"
                          v-model="config.touchpad_as_ds4"
                          default="true"
                ></Checkbox>
              </template>
              <template v-if="config.gamepad === 'ds4'">
                <Checkbox class="mb-3"
                          id="ds4_back_as_touchpad_click"
                          locale-prefix="config"
                          v-model="config.ds4_back_as_touchpad_click"
                          default="true"
                ></Checkbox>
              </template>
            </div>
          </div>
        </div>
      </template>
    </template>

    <!-- Home/Guide Button Emulation Timeout -->
    <div class="mb-4" v-if="config.controller === 'enabled'">
      <label for="back_button_timeout" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.back_button_timeout') }}</label>
      <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15" id="back_button_timeout" placeholder="-1"
             v-model="config.back_button_timeout" />
      <p class="text-[11px] opacity-60 mt-1">{{ $t('config.back_button_timeout_desc') }}</p>
    </div>

    <!-- Enable Keyboard Input -->
    <hr>
    <Checkbox class="mb-3"
              id="keyboard"
              locale-prefix="config"
              v-model="config.keyboard"
              default="true"
    ></Checkbox>

    <!-- Key Repeat Delay-->
    <div class="mb-4" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_delay" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.key_repeat_delay') }}</label>
      <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15" id="key_repeat_delay" placeholder="500"
             v-model="config.key_repeat_delay" />
      <p class="text-[11px] opacity-60 mt-1">{{ $t('config.key_repeat_delay_desc') }}</p>
    </div>

    <!-- Key Repeat Frequency-->
    <div class="mb-4" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_frequency" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{ $t('config.key_repeat_frequency') }}</label>
      <input type="text" class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15" id="key_repeat_frequency" placeholder="24.9"
             v-model="config.key_repeat_frequency" />
      <p class="text-[11px] opacity-60 mt-1">{{ $t('config.key_repeat_frequency_desc') }}</p>
    </div>

    <!-- Always send scancodes -->
    <Checkbox v-if="config.keyboard === 'enabled' && platform === 'windows'"
              class="mb-3"
              id="always_send_scancodes"
              locale-prefix="config"
              v-model="config.always_send_scancodes"
              default="true"
    ></Checkbox>

    <!-- Mapping Key AltRight to Key Windows -->
    <Checkbox v-if="config.keyboard === 'enabled'"
              class="mb-3"
              id="key_rightalt_to_key_win"
              locale-prefix="config"
              v-model="config.key_rightalt_to_key_win"
              default="false"
    ></Checkbox>

    <!-- Enable Mouse Input -->
    <hr>
    <Checkbox class="mb-3"
              id="mouse"
              locale-prefix="config"
              v-model="config.mouse"
              default="true"
    ></Checkbox>

    <!-- High resolution scrolling support -->
    <Checkbox v-if="config.mouse === 'enabled'"
              class="mb-3"
              id="high_resolution_scrolling"
              locale-prefix="config"
              v-model="config.high_resolution_scrolling"
              default="true"
    ></Checkbox>

    <!-- Native pen/touch support -->
    <Checkbox v-if="config.mouse === 'enabled'"
              class="mb-3"
              id="native_pen_touch"
              locale-prefix="config"
              v-model="config.native_pen_touch"
              default="true"
    ></Checkbox>
  </div>
</template>

<style scoped>

</style>
