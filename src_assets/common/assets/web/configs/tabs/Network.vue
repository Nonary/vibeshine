<script setup>
import { computed, ref } from 'vue'
import Checkbox from '@/Checkbox.vue'
import { useConfigStore } from '@/stores/config.js'

const store = useConfigStore()
const defaultMoonlightPort = 47989
const config = store.config
const effectivePort = computed(() => +config.value?.port ?? defaultMoonlightPort)
</script>

<template>
  <div id="network" class="config-page">
    <!-- UPnP -->
    <Checkbox id="upnp" v-model="config.upnp" class="mb-3" locale-prefix="config" default="false" />

    <!-- Address family -->
    <div class="mb-6">
      <label for="address_family" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.address_family') }}</label>
      <select id="address_family" v-model="config.address_family"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40">
        <option value="ipv4">
          {{ $t('config.address_family_ipv4') }}
        </option>
        <option value="both">
          {{ $t('config.address_family_both') }}
        </option>
      </select>
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.address_family_desc') }}
      </p>
    </div>

    <!-- Port family -->
    <div class="mb-6">
      <label for="port" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.port') }}</label>
      <input id="port" v-model="config.port" type="number" min="1029" max="65514"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70"
        :placeholder="defaultMoonlightPort">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.port_desc') }}
      </div>
      <!-- Add warning if any port is less than 1024 -->
      <div v-if="(+effectivePort - 5) < 1024"
        class="mt-2 rounded-md bg-red-50 text-red-800 border border-red-100 p-2 flex items-start gap-2">
        <i class="fa-solid fa-xl fa-triangle-exclamation" />
        <div class="text-sm">
          {{ $t('config.port_alert_1') }}
        </div>
      </div>
      <!-- Add warning if any port is above 65535 -->
      <div v-if="(+effectivePort + 21) > 65535"
        class="mt-2 rounded-md bg-red-50 text-red-800 border border-red-100 p-2 flex items-start gap-2">
        <i class="fa-solid fa-xl fa-triangle-exclamation" />
        <div class="text-sm">
          {{ $t('config.port_alert_2') }}
        </div>
      </div>
      <!-- Create a port table for the various ports needed by Sunshine -->
      <div class="mt-4 grid grid-cols-12 gap-2 text-sm">
        <div class="col-span-4 font-semibold">
          {{ $t('config.port_protocol') }}
        </div>
        <div class="col-span-4 font-semibold">
          {{ $t('config.port_port') }}
        </div>
        <div class="col-span-4 font-semibold">
          {{ $t('config.port_note') }}
        </div>

        <div class="col-span-4">
          {{ $t('config.port_tcp') }}
        </div>
        <div class="col-span-4">
          {{ +effectivePort - 5 }}
        </div>
        <div class="col-span-4" />

        <div class="col-span-4">
          {{ $t('config.port_tcp') }}
        </div>
        <div class="col-span-4">
          {{ +effectivePort }}
        </div>
        <div class="col-span-4">
          <div v-if="+effectivePort !== defaultMoonlightPort"
            class="mt-1 rounded-md bg-sky-50 text-sky-800 border border-sky-100 p-2">
            <i class="fa-solid fa-xl fa-circle-info" /> {{ $t('config.port_http_port_note') }}
          </div>
        </div>

        <div class="col-span-4">
          {{ $t('config.port_tcp') }}
        </div>
        <div class="col-span-4">
          {{ +effectivePort + 1 }}
        </div>
        <div class="col-span-4">
          {{ $t('config.port_web_ui') }}
        </div>

        <div class="col-span-4">
          {{ $t('config.port_tcp') }}
        </div>
        <div class="col-span-4">
          {{ +effectivePort + 21 }}
        </div>
        <div class="col-span-4" />

        <div class="col-span-4">
          {{ $t('config.port_udp') }}
        </div>
        <div class="col-span-4">
          {{ +effectivePort + 9 }} - {{ +effectivePort + 11 }}
        </div>
        <div class="col-span-4" />
      </div>
      <!-- add warning about exposing web ui to the internet -->
      <div v-if="config.origin_web_ui_allowed === 'wan'"
        class="mt-3 rounded-md bg-yellow-50 text-yellow-800 border border-yellow-100 p-2 flex items-start gap-2">
        <i class="fa-solid fa-xl fa-triangle-exclamation" /> {{ $t('config.port_warning') }}
      </div>
    </div>

    <!-- Origin Web UI Allowed -->
    <div class="mb-6">
      <label for="origin_web_ui_allowed" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.origin_web_ui_allowed') }}</label>
      <select id="origin_web_ui_allowed" v-model="config.origin_web_ui_allowed"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40">
        <option value="pc">
          {{ $t('config.origin_web_ui_allowed_pc') }}
        </option>
        <option value="lan">
          {{ $t('config.origin_web_ui_allowed_lan') }}
        </option>
        <option value="wan">
          {{ $t('config.origin_web_ui_allowed_wan') }}
        </option>
      </select>
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.origin_web_ui_allowed_desc') }}
      </p>
    </div>

    <!-- External IP -->
    <div class="mb-6">
      <label for="external_ip" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.external_ip') }}</label>
      <input id="external_ip" v-model="config.external_ip" type="text"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70"
        placeholder="123.456.789.12">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.external_ip_desc') }}
      </div>
    </div>

    <!-- LAN Encryption Mode -->
    <div class="mb-6">
      <label for="lan_encryption_mode" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.lan_encryption_mode') }}</label>
      <select id="lan_encryption_mode" v-model="config.lan_encryption_mode"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40">
        <option value="0">
          {{ $t('_common.disabled_def') }}
        </option>
        <option value="1">
          {{ $t('config.lan_encryption_mode_1') }}
        </option>
        <option value="2">
          {{ $t('config.lan_encryption_mode_2') }}
        </option>
      </select>
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.lan_encryption_mode_desc') }}
      </p>
    </div>

    <!-- WAN Encryption Mode -->
    <div class="mb-6">
      <label for="wan_encryption_mode" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.wan_encryption_mode') }}</label>
      <select id="wan_encryption_mode" v-model="config.wan_encryption_mode"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40">
        <option value="0">
          {{ $t('_common.disabled') }}
        </option>
        <option value="1">
          {{ $t('config.wan_encryption_mode_1') }}
        </option>
        <option value="2">
          {{ $t('config.wan_encryption_mode_2') }}
        </option>
      </select>
      <p class="text-[11px] opacity-60 mt-1">
        {{ $t('config.wan_encryption_mode_desc') }}
      </p>
    </div>

    <!-- Ping Timeout -->
    <div class="mb-6">
      <label for="ping_timeout" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.ping_timeout') }}</label>
      <input id="ping_timeout" v-model="config.ping_timeout" type="text"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70"
        placeholder="10000">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.ping_timeout_desc') }}
      </div>
    </div>
  </div>
</template>

<style scoped></style>
