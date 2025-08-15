<script setup>
import { ref } from 'vue';
import { $tp } from '@/platform-i18n';
import PlatformLayout from '@/PlatformLayout.vue';
import AdapterNameSelector from '@/configs/tabs/audiovideo/AdapterNameSelector.vue';
import DisplayOutputSelector from '@/configs/tabs/audiovideo/DisplayOutputSelector.vue';
import DisplayDeviceOptions from '@/configs/tabs/audiovideo/DisplayDeviceOptions.vue';
import DisplayModesSettings from '@/configs/tabs/audiovideo/DisplayModesSettings.vue';
import Checkbox from '@/Checkbox.vue';
import { useConfigStore } from '@/stores/config.js';
import { computed } from 'vue';

const store = useConfigStore();
const config = store.config;
const platform = computed(() => config.value?.platform || '');
</script>

<template>
  <div id="av" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-6">
      <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
      <input id="audio_sink" v-model="config.audio_sink" type="text" class="form-control"
        :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')" />
      <div class="text-[11px] opacity-60 mt-1">
        {{ $tp('config.audio_sink_desc') }}<br />
        <PlatformLayout>
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br />
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>

    <PlatformLayout>
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-6">
          <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
          <input id="virtual_sink" v-model="config.virtual_sink" type="text" class="form-control"
            :placeholder="$t('config.virtual_sink_placeholder')" />
          <div class="text-[11px] opacity-60 mt-1">
            {{ $t('config.virtual_sink_desc') }}
          </div>
        </div>

        <!-- Install Steam Audio Drivers -->
        <Checkbox id="install_steam_audio_drivers" v-model="config.install_steam_audio_drivers" class="mb-3"
          locale-prefix="config" default="true" />
      </template>
    </PlatformLayout>

    <!-- Disable Audio -->
    <Checkbox id="stream_audio" v-model="config.stream_audio" class="mb-3" locale-prefix="config" default="true" />

    <AdapterNameSelector />

    <DisplayOutputSelector />

    <DisplayDeviceOptions />

    <!-- Display Modes -->
    <DisplayModesSettings />
  </div>
</template>

<style scoped></style>
