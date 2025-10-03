<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { storeToRefs } from 'pinia'
import { useI18n } from 'vue-i18n'
import { NButton, NInput, NInputNumber, NSelect, NCheckbox } from 'naive-ui'
import Checkbox from '@/Checkbox.vue'
import { useConfigStore } from '@/stores/config'

const store = useConfigStore()
const { config, metadata } = storeToRefs(store)
const { t } = useI18n()

const platform = computed(
  () => (metadata.value?.platform || config.value?.platform || '').toString().toLowerCase()
)

const localeOptions = [
  { label: 'Български (Bulgarian)', value: 'bg' },
  { label: 'Čeština (Czech)', value: 'cs' },
  { label: 'Deutsch (German)', value: 'de' },
  { label: 'English', value: 'en' },
  { label: 'English, UK', value: 'en_GB' },
  { label: 'English, US', value: 'en_US' },
  { label: 'Español (Spanish)', value: 'es' },
  { label: 'Français (French)', value: 'fr' },
  { label: 'Italiano (Italian)', value: 'it' },
  { label: '日本語 (Japanese)', value: 'ja' },
  { label: '한국어 (Korean)', value: 'ko' },
  { label: 'Polski (Polish)', value: 'pl' },
  { label: 'Português (Portuguese)', value: 'pt' },
  { label: 'Português, Brasileiro (Portuguese, Brazilian)', value: 'pt_BR' },
  { label: 'Русский (Russian)', value: 'ru' },
  { label: 'svenska (Swedish)', value: 'sv' },
  { label: 'Türkçe (Turkish)', value: 'tr' },
  { label: 'Українська (Ukranian)', value: 'uk' },
  { label: '简体中文 (Chinese Simplified)', value: 'zh' },
  { label: '繁體中文 (Chinese Traditional)', value: 'zh_TW' }
]

const logLevelOptions = computed(() =>
  [0, 1, 2, 3, 4, 5, 6].map((v) => ({ label: t(`config.min_log_level_${v}`), value: v }))
)

function ensureCommandArray<T extends Record<string, any>>(key: 'global_prep_cmd' | 'global_state_cmd' | 'server_cmd'): T[] {
  if (!config.value) return [] as T[]
  const current = (config.value as any)[key]
  if (!Array.isArray(current)) {
    ;(config.value as any)[key] = []
    return (config.value as any)[key]
  }
  return current
}

function addPrepCommand() {
  if (!config.value) return
  const arr = ensureCommandArray('global_prep_cmd')
  arr.push({
    do: '',
    undo: '',
    elevated: platform.value === 'windows' ? false : !!arr[0]?.elevated
  })
  store.markManualDirty?.('global_prep_cmd')
}

function removePrepCommand(index: number) {
  if (!config.value) return
  const arr = ensureCommandArray('global_prep_cmd')
  if (index < 0 || index >= arr.length) return
  arr.splice(index, 1)
  store.markManualDirty?.('global_prep_cmd')
}

function addStateCommand() {
  if (!config.value) return
  const arr = ensureCommandArray('global_state_cmd')
  arr.push({
    do: '',
    undo: '',
    elevated: platform.value === 'windows' ? false : !!arr[0]?.elevated
  })
  store.markManualDirty?.('global_state_cmd')
}

function removeStateCommand(index: number) {
  if (!config.value) return
  const arr = ensureCommandArray('global_state_cmd')
  if (index < 0 || index >= arr.length) return
  arr.splice(index, 1)
  store.markManualDirty?.('global_state_cmd')
}

function addServerCommand() {
  if (!config.value) return
  const arr = ensureCommandArray('server_cmd')
  arr.push({
    name: '',
    cmd: '',
    elevated: platform.value === 'windows' ? false : !!arr[0]?.elevated
  })
  store.markManualDirty?.('server_cmd')
}

function removeServerCommand(index: number) {
  if (!config.value) return
  const arr = ensureCommandArray('server_cmd')
  if (index < 0 || index >= arr.length) return
  arr.splice(index, 1)
  store.markManualDirty?.('server_cmd')
}

function markManual(key: 'global_prep_cmd' | 'global_state_cmd' | 'server_cmd') {
  store.markManualDirty?.(key)
}

onMounted(() => {
  ensureCommandArray('global_prep_cmd')
  ensureCommandArray('global_state_cmd')
  ensureCommandArray('server_cmd')
})
</script>

<template>
  <div class="config-page space-y-6">
    <section class="space-y-4">
      <div>
        <label for="locale" class="form-label">{{ $t('config.locale') }}</label>
        <n-select
          id="locale"
          v-model:value="config.locale"
          :options="localeOptions"
          :data-search-options="localeOptions.map((o) => `${o.label}::${o.value ?? ''}`).join('|')"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.locale_desc') }}</p>
      </div>

      <div>
        <label for="sunshine_name" class="form-label">{{ $t('config.sunshine_name') }}</label>
        <n-input id="sunshine_name" v-model:value="config.sunshine_name" type="text" placeholder="Sunshine" />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.sunshine_name_desc') }}</p>
      </div>

      <div>
        <label for="min_log_level" class="form-label">{{ $t('config.min_log_level') }}</label>
        <n-select
          id="min_log_level"
          v-model:value="config.min_log_level"
          :options="logLevelOptions.map((o) => ({ ...o, label: $t(`config.min_log_level_${o.value}`) }))"
          :data-search-options="logLevelOptions.map((o) => `${$t(`config.min_log_level_${o.value}`)}::${o.value}`).join('|')"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.min_log_level_desc') }}</p>
      </div>

      <div>
        <label for="session_token_ttl_seconds" class="form-label">{{
          $t('config.session_token_ttl_seconds')
        }}</label>
        <n-input-number
          id="session_token_ttl_seconds"
          v-model:value="config.session_token_ttl_seconds"
          :min="60"
          :step="60"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.session_token_ttl_seconds_desc') }}</p>
      </div>

      <div>
        <label for="update_check_interval" class="form-label">{{
          $t('config.update_check_interval')
        }}</label>
        <n-input-number
          id="update_check_interval"
          v-model:value="config.update_check_interval"
          :min="0"
          :step="60"
        />
        <p class="text-[11px] opacity-60 mt-1">{{ $t('config.update_check_interval_desc') }}</p>
      </div>
    </section>

    <section class="space-y-4">
      <Checkbox id="notify_pre_releases" v-model="config.notify_pre_releases" locale-prefix="config" default="false" />
      <Checkbox id="system_tray" v-model="config.system_tray" locale-prefix="config" default="true" />
      <Checkbox
        id="hide_tray_controls"
        v-model="config.hide_tray_controls"
        locale-prefix="config"
        default="false"
      />
      <Checkbox id="enable_pairing" v-model="config.enable_pairing" locale-prefix="config" default="true" />
      <Checkbox id="enable_discovery" v-model="config.enable_discovery" locale-prefix="config" default="true" />
    </section>

    <section class="space-y-3">
      <header class="space-y-1">
        <h3 class="text-sm font-semibold">{{ $t('config.global_prep_cmd') }}</h3>
        <p class="text-[11px] opacity-60">{{ $t('config.global_prep_cmd_desc') }}</p>
      </header>

      <div v-if="ensureCommandArray('global_prep_cmd').length" class="space-y-3">
        <div
          v-for="(entry, index) in ensureCommandArray('global_prep_cmd')"
          :key="`prep-${index}`"
          class="rounded-lg border border-dark/10 dark:border-light/10 p-3 space-y-3"
        >
          <div class="flex items-center justify-between gap-2">
            <span class="text-xs opacity-70">Step {{ index + 1 }}</span>
            <div class="flex items-center gap-2">
              <n-checkbox
                v-if="platform === 'windows'"
                size="small"
                v-model:checked="entry.elevated"
                @update:checked="markManual('global_prep_cmd')"
              >
                {{ $t('_common.elevated') }}
              </n-checkbox>
              <n-button size="small" quaternary @click="removePrepCommand(index)">
                <i class="fas fa-trash" />
              </n-button>
            </div>
          </div>
          <div class="grid md:grid-cols-2 gap-3">
            <div>
              <span class="text-[11px] opacity-60">{{ $t('_common.do_cmd') }}</span>
              <n-input
                v-model:value="entry.do"
                type="textarea"
                :autosize="{ minRows: 1, maxRows: 4 }"
                @update:value="markManual('global_prep_cmd')"
              />
            </div>
            <div>
              <span class="text-[11px] opacity-60">{{ $t('_common.undo_cmd') }}</span>
              <n-input
                v-model:value="entry.undo"
                type="textarea"
                :autosize="{ minRows: 1, maxRows: 4 }"
                @update:value="markManual('global_prep_cmd')"
              />
            </div>
          </div>
        </div>
      </div>

      <div class="flex justify-end">
        <n-button type="primary" tertiary size="small" @click="addPrepCommand">
          + {{ $t('config.add') }}
        </n-button>
      </div>
    </section>

    <section class="space-y-3">
      <header class="space-y-1">
        <h3 class="text-sm font-semibold">{{ $t('config.global_state_cmd') }}</h3>
        <p class="text-[11px] opacity-60 whitespace-pre-line">{{ $t('config.global_state_cmd_desc') }}</p>
      </header>

      <div v-if="ensureCommandArray('global_state_cmd').length" class="space-y-3">
        <div
          v-for="(entry, index) in ensureCommandArray('global_state_cmd')"
          :key="`state-${index}`"
          class="rounded-lg border border-dark/10 dark:border-light/10 p-3 space-y-3"
        >
          <div class="flex items-center justify-between gap-2">
            <span class="text-xs opacity-70">Step {{ index + 1 }}</span>
            <div class="flex items-center gap-2">
              <n-checkbox
                v-if="platform === 'windows'"
                size="small"
                v-model:checked="entry.elevated"
                @update:checked="markManual('global_state_cmd')"
              >
                {{ $t('_common.elevated') }}
              </n-checkbox>
              <n-button size="small" quaternary @click="removeStateCommand(index)">
                <i class="fas fa-trash" />
              </n-button>
            </div>
          </div>
          <div class="grid md:grid-cols-2 gap-3">
            <div>
              <span class="text-[11px] opacity-60">{{ $t('_common.do_cmd') }}</span>
              <n-input
                v-model:value="entry.do"
                type="textarea"
                :autosize="{ minRows: 1, maxRows: 4 }"
                @update:value="markManual('global_state_cmd')"
              />
            </div>
            <div>
              <span class="text-[11px] opacity-60">{{ $t('_common.undo_cmd') }}</span>
              <n-input
                v-model:value="entry.undo"
                type="textarea"
                :autosize="{ minRows: 1, maxRows: 4 }"
                @update:value="markManual('global_state_cmd')"
              />
            </div>
          </div>
        </div>
      </div>

      <div class="flex justify-end">
        <n-button type="primary" tertiary size="small" @click="addStateCommand">
          + {{ $t('config.add') }}
        </n-button>
      </div>
    </section>

    <section class="space-y-3">
      <header class="space-y-1">
        <h3 class="text-sm font-semibold">{{ $t('config.server_cmd') }}</h3>
        <p class="text-[11px] opacity-60">{{ $t('config.server_cmd_desc') }}</p>
      </header>

      <div v-if="ensureCommandArray('server_cmd').length" class="space-y-3">
        <div
          v-for="(entry, index) in ensureCommandArray('server_cmd')"
          :key="`server-${index}`"
          class="rounded-lg border border-dark/10 dark:border-light/10 p-3 space-y-3"
        >
          <div class="flex items-center justify-between gap-2">
            <span class="text-xs opacity-70">Step {{ index + 1 }}</span>
            <div class="flex items-center gap-2">
              <n-checkbox
                v-if="platform === 'windows'"
                size="small"
                v-model:checked="entry.elevated"
                @update:checked="markManual('server_cmd')"
              >
                {{ $t('_common.elevated') }}
              </n-checkbox>
              <n-button size="small" quaternary @click="removeServerCommand(index)">
                <i class="fas fa-trash" />
              </n-button>
            </div>
          </div>
          <div class="grid md:grid-cols-2 gap-3">
            <div>
              <span class="text-[11px] opacity-60">{{ $t('_common.cmd_name') }}</span>
              <n-input
                v-model:value="entry.name"
                type="text"
                @update:value="markManual('server_cmd')"
              />
            </div>
            <div>
              <span class="text-[11px] opacity-60">{{ $t('_common.cmd_val') }}</span>
              <n-input
                v-model:value="entry.cmd"
                type="textarea"
                :autosize="{ minRows: 1, maxRows: 4 }"
                @update:value="markManual('server_cmd')"
              />
            </div>
          </div>
        </div>
      </div>

      <div class="flex justify-end">
        <n-button type="primary" tertiary size="small" @click="addServerCommand">
          + {{ $t('config.add') }}
        </n-button>
      </div>
    </section>
  </div>
</template>
