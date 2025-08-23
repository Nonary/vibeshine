<script setup>
import { ref, computed, onMounted } from 'vue';
import PlatformLayout from '@/PlatformLayout.vue';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { useI18n } from 'vue-i18n';
import { NInput, NInputNumber, NSelect, NButton, NModal, NCard, NTag } from 'naive-ui';
import { http } from '@/http';

const store = useConfigStore();
const config = store.config;
// Use platform from metadata to ensure OS-specific options load correctly.
const { metadata } = storeToRefs(store);
const platform = computed(() => metadata.value?.platform || '');
const { t } = useI18n();

const hevcModeOptions = [0, 1, 2, 3].map((v) => ({ labelKey: `config.hevc_mode_${v}`, value: v }));
const av1ModeOptions = [0, 1, 2, 3].map((v) => ({ labelKey: `config.av1_mode_${v}`, value: v }));

const captureOptions = computed(() => {
  const base = [{ label: t('_common.autodetect'), value: '' }];
  if (platform.value === 'linux') {
    base.push(
      { label: 'NvFBC', value: 'nvfbc' },
      { label: 'wlroots', value: 'wlr' },
      { label: 'KMS', value: 'kms' },
      { label: 'X11', value: 'x11' },
    );
  } else if (platform.value === 'windows') {
    base.push(
      { label: 'Desktop Duplication API', value: 'ddx' },
      { label: 'Windows.Graphics.Capture Variable Framerate', value: 'wgc' },
      { label: 'Windows.Graphics.Capture Constant Framerate', value: 'wgcc' },
    );
  }
  return base;
});

const encoderOptions = computed(() => {
  const opts = [{ label: t('_common.autodetect'), value: '' }];
  if (platform.value === 'windows') {
    opts.push(
      { label: 'NVIDIA NVENC', value: 'nvenc' },
      { label: 'Intel QuickSync', value: 'quicksync' },
      { label: 'AMD AMF/VCE', value: 'amdvce' },
    );
  } else if (platform.value === 'linux') {
    opts.push({ label: 'NVIDIA NVENC', value: 'nvenc' }, { label: 'VA-API', value: 'vaapi' });
  } else if (platform.value === 'macos') {
    opts.push({ label: 'VideoToolbox', value: 'videotoolbox' });
  }
  opts.push({ label: t('config.encoder_software'), value: 'software' });
  return opts;
});

// --- Playnite plugin uninstall (Windows only) ---
const showUninstallConfirm = ref(false);
const uninstalling = ref(false);
const uninstallOk = ref(false);
const uninstallErr = ref(false);
const uninstallErrorMsg = ref('');
const playniteInstalled = ref(false);
const playniteExtensionsDir = ref('');
const isWindows = computed(() => (platform.value || '').toLowerCase() === 'windows');

async function refreshPlayniteStatus() {
  if (!isWindows.value) return;
  try {
    const r = await http.get('/api/playnite/status', { validateStatus: () => true });
    const d = (r && r.data) || {};
    playniteInstalled.value = !!d.installed;
    playniteExtensionsDir.value = d.extensions_dir || '';
  } catch (_) {}
}

function openUninstallConfirm() {
  showUninstallConfirm.value = true;
}

async function confirmUninstall() {
  uninstalling.value = true;
  showUninstallConfirm.value = false;
  uninstallOk.value = false;
  uninstallErr.value = false;
  uninstallErrorMsg.value = '';
  try {
    // Backend may implement uninstall via install endpoint with a flag in future.
    const r = await http.post(
      '/api/playnite/uninstall',
      { restart: true },
      { validateStatus: () => true },
    );
    let ok = false;
    let body = null;
    try { body = r.data; } catch {}
    ok = r.status >= 200 && r.status < 300 && body && body.status === true;
    if (ok) {
      uninstallOk.value = true;
      await refreshPlayniteStatus();
    } else {
      uninstallErr.value = true;
      uninstallErrorMsg.value = (body && body.error) || r.statusText || 'Unknown error';
    }
  } catch (e) {
    uninstallErr.value = true;
    uninstallErrorMsg.value = (e && e.message) || '';
  }
  uninstalling.value = false;
}

onMounted(() => {
  refreshPlayniteStatus();
});
</script>

<template>
  <div class="config-page">
    <!-- Playnite Plugin (uninstall) -->
    <div v-if="isWindows && playniteExtensionsDir" class="mb-6">
      <label class="form-label flex items-center gap-2">
        <i class="fas fa-plug" />
        <span>{{ $t('navbar.playnite') }}</span>
        <n-tag v-if="playniteInstalled" size="small" type="default">{{ $t('playnite.status_installed') }}</n-tag>
      </label>
      <div class="flex items-center gap-2">
        <n-button
          v-if="playniteInstalled"
          size="small"
          type="error"
          secondary
          :loading="uninstalling"
          @click="openUninstallConfirm"
        >
          <i class="fas fa-trash" />
          <span class="ml-2">{{ $t('playnite.uninstall_button') || 'Uninstall Plugin' }}</span>
        </n-button>
        <span v-if="uninstallOk" class="text-success text-xs">{{ $t('playnite.uninstall_success') || 'Plugin uninstalled.' }}</span>
        <span v-if="uninstallErr" class="text-danger text-xs">{{ $t('playnite.uninstall_error') }}<template v-if="uninstallErrorMsg">: {{ uninstallErrorMsg }}</template></span>
      </div>
      <div class="form-text">{{ $t('playnite.extensions_dir') }}: <code class="break-all">{{ playniteExtensionsDir }}</code></div>
    </div>
    <!-- FEC Percentage -->
    <div class="mb-6">
      <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
      <n-input-number
        id="fec_percentage"
        v-model:value="config.fec_percentage"
        :placeholder="'20'"
      />
      <div class="form-text">{{ $t('config.fec_percentage_desc') }}</div>
    </div>

    <!-- Quantization Parameter -->
    <div class="mb-6">
      <label for="qp" class="form-label">{{ $t('config.qp') }}</label>
  <n-input-number id="qp" v-model:value="config.qp" :placeholder="'28'" />
      <div class="form-text">{{ $t('config.qp_desc') }}</div>
    </div>

    <!-- Min Threads -->
    <div class="mb-6">
      <label for="min_threads" class="form-label">{{ $t('config.min_threads') }}</label>
      <n-input-number
        id="min_threads"
        v-model:value="config.min_threads"
        :placeholder="'2'"
        :min="1"
      />
      <div class="form-text">{{ $t('config.min_threads_desc') }}</div>
    </div>

    <!-- HEVC Support -->
    <div class="mb-6">
      <label for="hevc_mode" class="form-label">{{ $t('config.hevc_mode') }}</label>
      <n-select
        id="hevc_mode"
        v-model:value="config.hevc_mode"
        :options="hevcModeOptions.map(o => ({ label: $t(o.labelKey), value: o.value }))"
        :data-search-options="hevcModeOptions.map(o => `${$t(o.labelKey)}::${o.value}`).join('|')"
      />
      <div class="form-text">{{ $t('config.hevc_mode_desc') }}</div>
    </div>

    <!-- AV1 Support -->
    <div class="mb-6">
      <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
      <n-select
        id="av1_mode"
        v-model:value="config.av1_mode"
        :options="av1ModeOptions.map(o => ({ label: $t(o.labelKey), value: o.value }))"
        :data-search-options="av1ModeOptions.map(o => `${$t(o.labelKey)}::${o.value}`).join('|')"
      />
      <div class="form-text">{{ $t('config.av1_mode_desc') }}</div>
    </div>

    <!-- Capture -->
    <div v-if="platform !== 'macos'" class="mb-6">
      <label for="capture" class="form-label">{{ $t('config.capture') }}</label>
      <n-select
        id="capture"
        v-model:value="config.capture"
        :options="captureOptions"
        :data-search-options="captureOptions.map(o => `${o.label}::${o.value ?? ''}`).join('|')"
      />
      <div class="form-text">{{ $t('config.capture_desc') }}</div>
    </div>

    <!-- Encoder -->
    <div class="mb-6">
      <label for="encoder" class="form-label">{{ $t('config.encoder') }}</label>
      <n-select
        id="encoder"
        v-model:value="config.encoder"
        :options="encoderOptions"
        :data-search-options="encoderOptions.map(o => `${o.label}::${o.value ?? ''}`).join('|')"
      />
      <div class="form-text">{{ $t('config.encoder_desc') }}</div>
    </div>
  </div>

  <!-- Uninstall confirmation -->
  <n-modal :show="showUninstallConfirm" @update:show="(v) => (showUninstallConfirm = v)">
    <n-card :bordered="false" style="max-width: 32rem; width: 100%">
      <template #header>
        <div class="flex items-center gap-2">
          <i class="fas fa-trash" />
          <span>{{ $t('playnite.uninstall_button') || 'Uninstall Plugin' }}</span>
        </div>
      </template>
      <div class="text-sm">
        {{ $t('playnite.uninstall_requires_restart') || 'This will remove the Sunshine Playnite plugin and restart Playnite. Continue?' }}
      </div>
      <template #footer>
        <div class="w-full flex items-center justify-center gap-3">
          <n-button tertiary @click="showUninstallConfirm = false">{{ $t('_common.cancel') }}</n-button>
          <n-button type="error" :loading="uninstalling" @click="confirmUninstall">{{ $t('_common.continue') || 'Continue' }}</n-button>
        </div>
      </template>
    </n-card>
  </n-modal>
</template>

<style scoped></style>
