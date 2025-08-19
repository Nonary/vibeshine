<template>
  <div class="space-y-6">
    <p class="text-[13px] opacity-80 text-center max-w-2xl mx-auto leading-relaxed">
      {{ $t('resource_card.resources_desc') }}
    </p>

    <!-- Link tiles: vertical layout, no truncation, large icons -->
    <n-grid cols="24" x-gap="20" y-gap="20" responsive="screen">
      <n-gi :span="24" :s="12">
        <a
          href="https://app.lizardbyte.dev"
          target="_blank"
          class="group p-5 rounded-2xl border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 hover:bg-light/90 dark:hover:bg-surface/80 hover:shadow-md transition text-center flex flex-col items-center gap-3"
        >
          <span
            class="shrink-0 inline-flex items-center justify-center h-12 w-12 rounded-full bg-primary/15 text-primary"
            ><i class="fas fa-globe text-[18px]"
          /></span>
          <div class="space-y-1">
            <div class="text-base font-semibold leading-snug">
              {{ $t('resource_card.lizardbyte_website') }}
            </div>
            <div class="text-sm opacity-70 leading-snug">app.lizardbyte.dev</div>
          </div>
        </a>
      </n-gi>

      <n-gi :span="24" :s="12">
        <a
          href="https://app.lizardbyte.dev/discord"
          target="_blank"
          class="group p-5 rounded-2xl border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 hover:bg-light/90 dark:hover:bg-surface/80 hover:shadow-md transition text-center flex flex-col items-center gap-3"
        >
          <span
            class="shrink-0 inline-flex items-center justify-center h-12 w-12 rounded-full bg-accent/15 text-accent"
            ><i class="fab fa-discord text-[18px]"
          /></span>
          <div class="space-y-1">
            <div class="text-base font-semibold leading-snug">Discord</div>
            <div class="text-sm opacity-70 leading-snug">Join the community</div>
          </div>
        </a>
      </n-gi>

      <n-gi :span="24" :s="12">
        <a
          href="https://github.com/orgs/LizardByte/discussions"
          target="_blank"
          class="group p-5 rounded-2xl border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 hover:bg-light/90 dark:hover:bg-surface/80 hover:shadow-md transition text-center flex flex-col items-center gap-3"
        >
          <span
            class="shrink-0 inline-flex items-center justify-center h-12 w-12 rounded-full bg-brand/15 text-brand"
            ><i class="fab fa-github text-[18px]"
          /></span>
          <div class="space-y-1">
            <div class="text-base font-semibold leading-snug">
              {{ $t('resource_card.github_discussions') }}
            </div>
            <div class="text-sm opacity-70 leading-snug">GitHub Discussions</div>
          </div>
        </a>
      </n-gi>
    </n-grid>

    <div class="border-t border-dark/10 dark:border-light/10 pt-6 space-y-3">
      <h3 class="text-sm font-semibold text-center">{{ $t('resource_card.legal') }}</h3>
      <p class="text-xs opacity-70 text-center max-w-xl mx-auto leading-relaxed">
        {{ $t('resource_card.legal_desc') }}
      </p>
      <n-grid cols="24" x-gap="20" y-gap="20" responsive="screen">
        <n-gi :span="24" :s="12">
          <a
            href="https://github.com/LizardByte/Sunshine/blob/master/LICENSE"
            target="_blank"
            class="group p-5 rounded-2xl border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 hover:bg-light/90 dark:hover:bg-surface/80 hover:shadow-md transition text-center flex flex-col items-center gap-3"
          >
            <span
              class="shrink-0 inline-flex items-center justify-center h-12 w-12 rounded-full bg-success/15 text-success"
              ><i class="fas fa-file-alt text-[18px]"
            /></span>
            <div class="space-y-1">
              <div class="text-base font-semibold leading-snug">
                {{ $t('resource_card.license') }}
              </div>
              <div class="text-sm opacity-70 leading-snug">Sunshine License</div>
            </div>
          </a>
        </n-gi>

        <n-gi :span="24" :s="12">
          <a
            href="https://github.com/LizardByte/Sunshine/blob/master/NOTICE"
            target="_blank"
            class="group p-5 rounded-2xl border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 hover:bg-light/90 dark:hover:bg-surface/80 hover:shadow-md transition text-center flex flex-col items-center gap-3"
          >
            <span
              class="shrink-0 inline-flex items-center justify-center h-12 w-12 rounded-full bg-danger/15 text-danger"
              ><i class="fas fa-exclamation text-[18px]"
            /></span>
            <div class="space-y-1">
              <div class="text-base font-semibold leading-snug">
                {{ $t('resource_card.third_party_notice') }}
              </div>
              <div class="text-sm opacity-70 leading-snug">Third-party notices</div>
            </div>
          </a>
        </n-gi>
      </n-grid>
    </div>
  </div>
</template>

<script setup lang="ts">
import { NGrid, NGi } from 'naive-ui';
</script>
