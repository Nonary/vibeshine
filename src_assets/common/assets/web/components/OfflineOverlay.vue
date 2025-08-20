<template>
  <transition name="fade-fast">
    <div v-if="visible" class="fixed inset-0 z-[140] flex flex-col">
      <div
        class="absolute inset-0 bg-gradient-to-br from-white/70 via-white/60 to-white/70 dark:from-black/70 dark:via-black/60 dark:to-black/70 backdrop-blur-md"
      ></div>
      <div class="relative flex-1 flex flex-col items-center justify-center p-6 overflow-y-auto">
        <div class="w-full max-w-md mx-auto text-center space-y-6">
          <img
            src="/images/logo-sunshine-45.png"
            alt="Sunshine"
            class="h-24 w-24 opacity-80 mx-auto select-none"
          />
          <div class="space-y-2">
            <h2 class="text-2xl font-semibold tracking-tight">
              {{ $t('offline.title') }}
            </h2>
            <p class="text-sm opacity-80 leading-relaxed">
              {{ $t('offline.description') }}
            </p>
            <p class="text-xs opacity-70 leading-relaxed">
              {{ $t('offline.retrying') }}
            </p>
          </div>
          <div class="flex items-center justify-center pt-2 gap-3">
            <n-button type="primary" @click="refreshNow">
              {{ $t('offline.refresh_now') }}
              <i class="fas fa-rotate" />
            </n-button>
          </div>
          <p class="mt-8 text-[10px] tracking-wider uppercase opacity-60 select-none">
            {{ $t('offline.close_hint') }}
          </p>
        </div>
      </div>
    </div>
  </transition>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useConnectivityStore } from '@/stores/connectivity';
import { NButton } from 'naive-ui';

const connectivity = useConnectivityStore();
const visible = computed(() => connectivity.offline);

function refreshNow() {
  try {
    window.location.reload();
  } catch {}
}
</script>

