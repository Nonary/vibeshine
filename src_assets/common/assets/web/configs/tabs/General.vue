<script setup>
import Checkbox from '@/Checkbox.vue'
import { ref } from 'vue'
import { useConfigStore } from '@/stores/config.js'

const store = useConfigStore()
const config = store.config
import { computed } from 'vue'
const platform = computed(() => config.value?.platform || '')

function addCmd() {
  let template = { do: "", undo: "" }
  if (store) {
    if (store.config && store.config.platform === 'windows') template = { ...template, elevated: false }
    // ensure array
    if (!config.value.global_prep_cmd) config.value.global_prep_cmd = []
    config.value.global_prep_cmd.push(template)
  }
}

function removeCmd(index) {
  if (config.value && config.value.global_prep_cmd) config.value.global_prep_cmd.splice(index, 1)
}
</script>

<template>
  <div id="general" class="config-page">
    <!-- Locale -->
    <div class="mb-6">
      <label for="locale" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.locale') }}</label>
      <select id="locale" v-model="config.locale"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 text-solar-dark dark:text-lunar-light placeholder-black/40 dark:placeholder-lunar-light/40 focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40">
        <option value="bg">
          Български (Bulgarian)
        </option>
        <option value="cs">
          Čeština (Czech)
        </option>
        <option value="de">
          Deutsch (German)
        </option>
        <option value="en">
          English
        </option>
        <option value="en_GB">
          English, UK
        </option>
        <option value="en_US">
          English, US
        </option>
        <option value="es">
          Español (Spanish)
        </option>
        <option value="fr">
          Français (French)
        </option>
        <option value="it">
          Italiano (Italian)
        </option>
        <option value="ja">
          日本語 (Japanese)
        </option>
        <option value="ko">
          한국어 (Korean)
        </option>
        <option value="pl">
          Polski (Polish)
        </option>
        <option value="pt">
          Português (Portuguese)
        </option>
        <option value="pt_BR">
          Português, Brasileiro (Portuguese, Brazilian)
        </option>
        <option value="ru">
          Русский (Russian)
        </option>
        <option value="sv">
          svenska (Swedish)
        </option>
        <option value="tr">
          Türkçe (Turkish)
        </option>
        <option value="uk">
          Українська (Ukranian)
        </option>
        <option value="zh">
          简体中文 (Chinese Simplified)
        </option>
        <option value="zh_TW">
          繁體中文 (Chinese Traditional)
        </option>
      </select>
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.locale_desc') }}
      </div>
    </div>

    <!-- Sunshine Name -->
    <div class="mb-6">
      <label for="sunshine_name" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.sunshine_name') }}</label>
      <input id="sunshine_name" v-model="config.sunshine_name" type="text"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 text-solar-dark dark:text-lunar-light focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40"
        placeholder="Sunshine">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.sunshine_name_desc') }}
      </div>
    </div>

    <!-- Log Level -->
    <div class="mb-6">
      <label for="min_log_level" class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.log_level') }}</label>
      <select id="min_log_level" v-model="config.min_log_level"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70 text-solar-dark dark:text-lunar-light focus:outline-none focus:ring-2 focus:ring-solar-primary/40 dark:focus:ring-lunar-primary/40">
        <option value="0">
          {{ $t('config.log_level_0') }}
        </option>
        <option value="1">
          {{ $t('config.log_level_1') }}
        </option>
        <option value="2">
          {{ $t('config.log_level_2') }}
        </option>
        <option value="3">
          {{ $t('config.log_level_3') }}
        </option>
        <option value="4">
          {{ $t('config.log_level_4') }}
        </option>
        <option value="5">
          {{ $t('config.log_level_5') }}
        </option>
        <option value="6">
          {{ $t('config.log_level_6') }}
        </option>
      </select>
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.log_level_desc') }}
      </div>
    </div>

    <!-- Global Prep Commands -->
    <div id="global_prep_cmd" class="mb-6 flex flex-col">
      <label class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
        $t('config.global_prep_cmd') }}</label>
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.global_prep_cmd_desc') }}
      </div>
      <div v-if="config.global_prep_cmd.length > 0" class="mt-3 space-y-3">
        <div v-for="(c, i) in config.global_prep_cmd" :key="i" class="grid grid-cols-12 gap-2 items-start">
          <div class="col-span-5">
            <input v-model="c.do" type="text"
              class="w-full font-mono px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70">
          </div>
          <div class="col-span-5">
            <input v-model="c.undo" type="text"
              class="w-full font-mono px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70">
          </div>
          <div v-if="platform === 'windows'" class="col-span-1">
            <Checkbox :id="'prep-cmd-admin-' + i" v-model="c.elevated" label="_common.elevated" desc="" />
          </div>
          <div class="col-span-1 flex gap-2">
            <button class="inline-flex items-center px-3 py-1.5 rounded-md bg-solar-danger text-white"
              @click="removeCmd(i)">
              <i class="fas fa-trash" />
            </button>
            <button class="inline-flex items-center px-3 py-1.5 rounded-md bg-solar-success text-white" @click="addCmd">
              <i class="fas fa-plus" />
            </button>
          </div>
        </div>
      </div>
      <div class="mt-4">
        <button class="mx-auto block inline-flex items-center px-3 py-1.5 rounded-md bg-solar-success text-white"
          @click="addCmd">
          &plus; {{ $t('config.add') }}
        </button>
      </div>
    </div>

    <!-- Session Token TTL -->
    <div class="mb-6">
      <label for="session_token_ttl_seconds"
        class="block text-sm font-medium mb-1 text-solar-dark dark:text-lunar-light">{{
          $t('config.session_token_ttl_seconds') }}</label>
      <input id="session_token_ttl_seconds" v-model.number="config.session_token_ttl_seconds" type="number" min="60"
        step="60"
        class="w-full px-3 py-2 text-sm rounded-md border border-black/10 dark:border-white/15 bg-white dark:bg-lunar-surface/70">
      <div class="text-[11px] opacity-60 mt-1">
        {{ $t('config.session_token_ttl_seconds_desc') }}
      </div>
    </div>

    <!-- Notify Pre-Releases -->
    <Checkbox id="notify_pre_releases" v-model="config.notify_pre_releases" class="mb-3" locale-prefix="config"
      default="false" />
  </div>
</template>

<style scoped></style>
