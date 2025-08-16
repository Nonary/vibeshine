<template>
  <div class="space-y-8 max-w-5xl">
    <!-- Welcome / Intro -->
    <div>
      <h1 class="text-3xl font-semibold mb-2">{{ $t('index.welcome') }}</h1>
      <p class="text-sm opacity-80">{{ $t('index.description') }}</p>
    </div>

    <!-- Fatal startup errors -->
    <div v-if="fancyLogs.some((l) => l.level === 'Fatal')">
      <UiAlert variant="danger">
        <template #icon>
          <i class="fas fa-circle-exclamation text-xl" />
        </template>
        <div class="space-y-3">
          <p class="text-sm leading-relaxed" v-html="$t('index.startup_errors')"></p>
          <ul class="list-disc pl-5 space-y-1 text-xs">
            <li v-for="(v, i) in fancyLogs.filter((x) => x.level === 'Fatal')" :key="i">
              {{ v.value }}
            </li>
          </ul>
          <div>
            <a
              class="inline-flex items-center gap-2 px-3 py-1.5 rounded-md bg-danger-600 text-white text-xs hover:bg-danger-500 transition"
              href="./troubleshooting#logs"
            >
              <i class="fas fa-file-lines" /> {{ $t('index.view_logs') || 'View Logs' }}
            </a>
          </div>
        </div>
      </UiAlert>
    </div>

    <!-- Version Card -->
    <UiCard v-if="version" :title="'Version ' + version.version">
      <div class="space-y-4 text-sm">
        <div v-if="loading" class="text-xs italic flex items-center gap-2">
          <i class="fas fa-spinner animate-spin" /> {{ $t('index.loading_latest') }}
        </div>
        <div v-if="branch || commit" class="text-xs opacity-70 font-mono">
          {{ $t('index.version_branch', { branch, commit: commit.substring(0, 7) }) }}
        </div>
        <UiAlert v-if="buildVersionIsDirty" variant="success">
          <template #icon><i class="fas fa-seedling" /></template>
          {{ $t('index.version_dirty') }} 🌇
        </UiAlert>
        <UiAlert v-if="installedVersionNotStable" variant="info">
          <template #icon><i class="fas fa-flask" /></template>
          {{ $t('index.installed_version_not_stable') }}
        </UiAlert>
        <UiAlert v-if="!installedVersionNotStable && aheadByCommits > 0" variant="success">
          <template #icon><i class="fas fa-forward" /></template>
          {{ $t('index.version_ahead', { ahead: aheadByCommits }) }}
        </UiAlert>
        <UiAlert
          v-if="!installedVersionNotStable && aheadByCommits === 0 && behindByCommits > 0"
          variant="warning"
        >
          <template #icon><i class="fas fa-clock-rotate-left" /></template>
          {{ $t('index.version_behind', { behind: behindByCommits }) }}
        </UiAlert>
        <UiAlert
          v-if="
            !installedVersionNotStable &&
            !compareInfo &&
            !stableBuildAvailable &&
            !buildVersionIsDirty
          "
          variant="info"
        >
          <template #icon><i class="fas fa-circle-question" /></template>
          {{ $t('index.version_compare_unknown') }}
        </UiAlert>
        <UiAlert
          v-else-if="
            (!preReleaseBuildAvailable || !notifyPreReleases) &&
            !stableBuildAvailable &&
            !buildVersionIsDirty
          "
          variant="success"
        >
          <template #icon><i class="fas fa-check-circle" /></template>
          {{ $t('index.version_latest') }}
        </UiAlert>

        <!-- Pre-release notice -->
        <UiAlert v-if="notifyPreReleases && preReleaseBuildAvailable" variant="warning">
          <template #icon><i class="fas fa-bell" /></template>
          <div class="flex flex-col gap-3 w-full">
            <div class="flex flex-col md:flex-row md:items-center md:justify-between gap-3">
              <p class="text-sm m-0">{{ $t('index.new_pre_release') }}</p>
              <a
                class="UiButton UiButton--primary"
                :href="preReleaseVersion.release.html_url"
                target="_blank"
                >{{ $t('index.download') }}</a
              >
            </div>
            <div
              class="bg-dark/5 dark:bg-light/5 rounded p-3 overflow-auto max-h-64 text-xs font-mono"
            >
              <p class="font-semibold mb-2">{{ preReleaseVersion.release.name }}</p>
              <pre class="whitespace-pre-wrap">{{ preReleaseVersion.release.body }}</pre>
            </div>
          </div>
        </UiAlert>

        <!-- Stable update available -->
        <UiAlert v-if="stableBuildAvailable" variant="warning">
          <template #icon><i class="fas fa-arrow-up" /></template>
          <div class="flex flex-col gap-3 w-full">
            <div class="flex flex-col md:flex-row md:items-center md:justify-between gap-3">
              <p class="text-sm m-0">{{ $t('index.new_stable') }}</p>
              <a
                class="UiButton UiButton--primary"
                :href="githubVersion.release.html_url"
                target="_blank"
                >{{ $t('index.download') }}</a
              >
            </div>
            <div
              class="bg-dark/5 dark:bg-light/5 rounded p-3 overflow-auto max-h-64 text-xs font-mono"
            >
              <p class="font-semibold mb-2">{{ githubVersion.release.name }}</p>
              <pre class="whitespace-pre-wrap">{{ githubVersion.release.body }}</pre>
            </div>
          </div>
        </UiAlert>
      </div>
    </UiCard>

    <!-- Resources -->
    <UiCard :title="$t('resources.title') || 'Resources'">
      <div class="text-xs space-y-2">
        <ResourceCard />
      </div>
    </UiCard>
  </div>
</template>

<script setup>
import { ref, onMounted, computed } from 'vue';
import UiAlert from '@/components/UiAlert.vue';
import UiCard from '@/components/UiCard.vue';
import ResourceCard from '@/ResourceCard.vue';
import SunshineVersion from '@/sunshine_version';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';

const version = ref(null);
const githubVersion = ref(null);
const preReleaseVersion = ref(null);
const notifyPreReleases = ref(false);
const loading = ref(true);
const logs = ref('');
const branch = ref('');
const commit = ref('');
const compareInfo = ref(null); // {ahead_by, behind_by, status}

const configStore = useConfigStore();
const auth = useAuthStore();
let started = false; // prevent duplicate concurrent checks

async function runVersionChecks() {
  if (started) return; // guard
  started = true;
  loading.value = true;
  try {
    // Use config store (it already handles deep cloning & defaults)
    const cfg = await configStore.fetchConfig();
    if (!cfg) {
      // still not available (possibly lost auth); allow retry later
      started = false;
      loading.value = false;
      return;
    }
    // Normalize notify pre-release flag to boolean
    notifyPreReleases.value =
      cfg.notify_pre_releases === true || cfg.notify_pre_releases === 'enabled';
    version.value = new SunshineVersion(null, cfg.version);
    branch.value = cfg.branch || '';
    commit.value = cfg.commit || '';

    // Remote release checks (GitHub)
    try {
      githubVersion.value = new SunshineVersion(
        await fetch('https://api.github.com/repos/LizardByte/Sunshine/releases/latest').then((r) =>
          r.json(),
        ),
        null,
      );
    } catch (e) {
      // eslint-disable-next-line no-console
      console.warn('[Dashboard] latest release fetch failed', e);
    }
    try {
      const releases = await fetch(
        'https://api.github.com/repos/LizardByte/Sunshine/releases',
      ).then((r) => r.json());
      const pre = Array.isArray(releases) ? releases.find((r) => r.prerelease) : null;
      if (pre) preReleaseVersion.value = new SunshineVersion(pre, null);
    } catch (e) {
      // eslint-disable-next-line no-console
      console.warn('[Dashboard] releases list fetch failed', e);
    }

    // Compare if we have enough data
    if (commit.value && githubVersion.value?.version) {
      try {
        const baseTag = githubVersion.value.version.startsWith('v')
          ? githubVersion.value.version
          : 'v' + githubVersion.value.version;
        const compareResp = await fetch(
          `https://api.github.com/repos/LizardByte/Sunshine/compare/${baseTag}...${commit.value}`,
        );
        if (compareResp.ok) {
          const cmp = await compareResp.json();
          compareInfo.value = {
            ahead_by: cmp.ahead_by,
            behind_by: cmp.behind_by,
            status: cmp.status,
          };
        }
      } catch (e) {
        // eslint-disable-next-line no-console
        console.warn('Compare API failed', e);
      }
    }
  } catch (e) {
    // eslint-disable-next-line no-console
    console.error('[Dashboard] version checks failed', e);
  }
  try {
    // logs only after auth
    logs.value = await fetch('./api/logs').then((r) => r.text());
  } catch (e) {
    // eslint-disable-next-line no-console
    console.error('[Dashboard] logs fetch failed', e);
  }
  loading.value = false;
}

onMounted(() => {
  // If already authenticated (likely after bootstrap), run immediately; otherwise defer.
  if (auth.isAuthenticated) {
    runVersionChecks();
  } else {
    // Register one-time listener
    const off = auth.onLogin(() => {
      off && off();
      runVersionChecks();
    });
  }
});

const installedVersionNotStable = computed(() => {
  if (!githubVersion.value || !version.value) return false;
  // treat non-master branches as pre-release builds automatically
  if (branch.value && branch.value !== 'master') return true;
  return version.value.isGreater(githubVersion.value);
});
const stableBuildAvailable = computed(() => {
  if (!githubVersion.value || !version.value) return false;
  // If we have compare info and we're ahead, do not suggest stable update
  if (compareInfo.value && compareInfo.value.ahead_by > 0) return false;
  // If we have compare info and we're exactly equal (ahead_by = behind_by = 0) treat as up-to-date.
  if (compareInfo.value && compareInfo.value.ahead_by === 0 && compareInfo.value.behind_by === 0)
    return false;
  return githubVersion.value.isGreater(version.value);
});
const preReleaseBuildAvailable = computed(() => {
  if (!preReleaseVersion.value || !githubVersion.value || !version.value) return false;
  return (
    preReleaseVersion.value.isGreater(version.value) &&
    preReleaseVersion.value.isGreater(githubVersion.value)
  );
});
const buildVersionIsDirty = computed(() => {
  if (!version.value) return false;
  return (
    version.value.version?.split('.').length === 5 && version.value.version.indexOf('dirty') !== -1
  );
});
const aheadByCommits = computed(() => compareInfo.value?.ahead_by || 0);
const behindByCommits = computed(() => compareInfo.value?.behind_by || 0);
const fancyLogs = computed(() => {
  if (!logs.value) return [];
  const regex = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g;
  const rawLogLines = logs.value.split(regex).splice(1);
  const logLines = [];
  for (let i = 0; i < rawLogLines.length; i += 2) {
    logLines.push({
      timestamp: rawLogLines[i],
      level: rawLogLines[i + 1].split(':')[0],
      value: rawLogLines[i + 1],
    });
  }
  return logLines;
});
</script>

<style scoped></style>
