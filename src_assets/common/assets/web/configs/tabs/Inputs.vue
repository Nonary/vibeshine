<script setup lang="ts">
import { computed } from 'vue';
import { storeToRefs } from 'pinia';
import { NSelect, NInputNumber, NInput, NDivider } from 'naive-ui';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const { config, metadata } = storeToRefs(store);

const platform = computed(
  () => (metadata.value?.platform || config.value?.platform || '').toString().toLowerCase(),
);
const controllerEnabled = computed(() => (config.value?.controller || '') === 'enabled');
const gamepadValue = computed(() => config.value?.gamepad || 'auto');

const labelMap: Record<string, string> = {
  auto: '_common.auto',
  ds4: 'config.gamepad_ds4',
  ds5: 'config.gamepad_ds5',
  switch: 'config.gamepad_switch',
  x360: 'config.gamepad_x360',
  xone: 'config.gamepad_xone',
};

const prioritizedByPlatform: Record<string, string[]> = {
  linux: ['ds5', 'xone', 'switch', 'x360', 'ds4'],
  windows: ['x360', 'ds4'],
};

const fallbackOrder = ['x360', 'ds5', 'ds4'];

const gamepadOptions = computed(() => {
  const opts = [{ label: labelMap.auto, value: 'auto' }];
  const seen = new Set<string>(['auto']);
  const addOption = (value: string | undefined) => {
    if (!value || seen.has(value)) return;
    const label = labelMap[value] || `config.gamepad_${value}`;
    opts.push({ label, value });
    seen.add(value);
  };
  const order = prioritizedByPlatform[platform.value] ?? fallbackOrder;
  order.forEach(addOption);
  addOption(config.value?.gamepad);
  return opts;
});

const showAutoMotionOptions = computed(
  () => controllerEnabled.value && gamepadValue.value === 'auto' && ['windows', 'linux'].includes(platform.value),
);
const showDs4Options = computed(
  () =>
    controllerEnabled.value &&
    (gamepadValue.value === 'ds4' || (gamepadValue.value === 'auto' && platform.value === 'windows')),
);
const showDs5Options = computed(
  () =>
    controllerEnabled.value &&
    (gamepadValue.value === 'ds5' || (gamepadValue.value === 'auto' && platform.value === 'linux')),
);
</script>

<template>
  <div class="config-page space-y-6">
    <section class="space-y-4">
      <Checkbox id="controller" v-model="config.controller" locale-prefix="config" default="true" />

      <div v-if="controllerEnabled" class="space-y-2">
        <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
        <n-select
          id="gamepad"
          v-model:value="config.gamepad"
          :options="gamepadOptions.map((o) => ({ label: $t(o.label), value: o.value }))"
          :data-search-options="gamepadOptions.map((o) => `${$t(o.label)}::${o.value}`).join('|')"
        />
        <p class="text-[11px] opacity-60">{{ $t('config.gamepad_desc') }}</p>
      </div>

      <div v-if="controllerEnabled" class="space-y-3">
        <div v-if="showAutoMotionOptions" class="space-y-3">
          <Checkbox
            id="motion_as_ds4"
            v-model="config.motion_as_ds4"
            locale-prefix="config"
            default="true"
          />
          <Checkbox
            id="touchpad_as_ds4"
            v-model="config.touchpad_as_ds4"
            locale-prefix="config"
            default="true"
          />
        </div>

        <Checkbox
          v-if="showDs4Options"
          id="ds4_back_as_touchpad_click"
          v-model="config.ds4_back_as_touchpad_click"
          locale-prefix="config"
          default="true"
        />

        <Checkbox
          v-if="showDs5Options"
          id="ds5_inputtino_randomize_mac"
          v-model="config.ds5_inputtino_randomize_mac"
          locale-prefix="config"
          default="true"
        />

        <Checkbox
          id="forward_rumble"
          v-model="config.forward_rumble"
          locale-prefix="config"
          default="true"
        />

        <div class="space-y-2">
          <label for="back_button_timeout" class="form-label">{{
            $t('config.back_button_timeout')
          }}</label>
          <n-input-number
            id="back_button_timeout"
            v-model:value="config.back_button_timeout"
            :min="-1"
            :step="1"
          />
          <p class="text-[11px] opacity-60">{{ $t('config.back_button_timeout_desc') }}</p>
        </div>
      </div>
    </section>

    <n-divider />

    <section class="space-y-4">
      <h3 class="text-sm font-semibold">Keyboard</h3>

      <Checkbox id="keyboard" v-model="config.keyboard" locale-prefix="config" default="true" />

      <div class="grid md:grid-cols-2 gap-4">
        <div>
          <label for="key_repeat_delay" class="form-label">{{ $t('config.key_repeat_delay') }}</label>
          <n-input-number
            id="key_repeat_delay"
            v-model:value="config.key_repeat_delay"
            :min="0"
            :step="50"
          />
          <p class="text-[11px] opacity-60">{{ $t('config.key_repeat_delay_desc') }}</p>
        </div>
        <div>
          <label for="key_repeat_frequency" class="form-label">{{
            $t('config.key_repeat_frequency')
          }}</label>
          <n-input-number
            id="key_repeat_frequency"
            v-model:value="config.key_repeat_frequency"
            :min="0"
            :step="0.1"
          />
          <p class="text-[11px] opacity-60">{{ $t('config.key_repeat_frequency_desc') }}</p>
        </div>
      </div>

      <Checkbox
        id="always_send_scancodes"
        v-model="config.always_send_scancodes"
        locale-prefix="config"
        default="true"
      />
      <Checkbox
        id="key_rightalt_to_key_win"
        v-model="config.key_rightalt_to_key_win"
        locale-prefix="config"
        default="false"
      />
    </section>

    <n-divider />

    <section class="space-y-4">
      <h3 class="text-sm font-semibold">Pointer &amp; touch</h3>
      <Checkbox id="mouse" v-model="config.mouse" locale-prefix="config" default="true" />
      <Checkbox
        id="high_resolution_scrolling"
        v-model="config.high_resolution_scrolling"
        locale-prefix="config"
        default="true"
      />
      <Checkbox
        id="native_pen_touch"
        v-model="config.native_pen_touch"
        locale-prefix="config"
        default="true"
      />
      <Checkbox
        id="enable_input_only_mode"
        v-model="config.enable_input_only_mode"
        locale-prefix="config"
        default="false"
      />
    </section>

    <n-divider />

    <section class="space-y-3">
      <label for="keybindings" class="form-label">{{ $t('config.keybindings') }}</label>
      <n-input
        id="keybindings"
        v-model:value="config.keybindings"
        type="text"
        class="font-mono"
        placeholder="[0x10,0xA0,0x11,0xA2,0x12,0xA4]"
      />
      <p class="text-[11px] opacity-60">{{ $t('config.keybindings_desc') }}</p>
    </section>
  </div>
</template>
