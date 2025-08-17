<template>
  <div class="space-y-8 max-w-6xl mx-auto px-2 md:px-4">
    <!-- Hero / Intro -->
    <section
      class="rounded-xl border border-black/5 dark:border-white/10 bg-white/70 dark:bg-surface/70 backdrop-blur p-5 md:p-6 shadow-sm"
    >
      <div class="flex flex-col md:flex-row md:items-center md:justify-between gap-4">
        <div class="min-w-0">
          <h2 class="text-xl md:text-2xl font-semibold tracking-tight">
            {{ $t('index.welcome') }}
          </h2>
          <p class="text-sm opacity-80 mt-1 leading-relaxed">
            {{ $t('index.description') }}
          </p>
        </div>
        <div class="flex items-center gap-2 shrink-0">
          <RouterLink
            to="/settings"
            class="inline-flex items-center gap-2 px-3 py-1.5 rounded-lg bg-primary/80 text-onPrimary shadow hover:brightness-110 focus:outline-none focus-visible:ring-2 focus-visible:ring-offset-2 focus-visible:ring-primary/60 transition"
          >
            <i class="fas fa-sliders" />
            <span>Settings</span>
          </RouterLink>
          <RouterLink
            to="/applications"
            class="inline-flex items-center gap-2 px-3 py-1.5 rounded-lg border border-black/10 dark:border-white/10 hover:bg-black/5 dark:hover:bg-white/10 transition"
          >
            <i class="fas fa-grid-2" />
            <span>Applications</span>
          </RouterLink>
        </div>
      </div>
    </section>

    <!-- Fatal startup errors moved into Version card to avoid layout shift -->

    <!-- Main Grid -->
    <div class="grid grid-cols-1 xl:grid-cols-3 gap-6">
      <!-- Version Card -->
      <UiCard v-if="installedVersion" class="xl:col-span-2">
        <template #title>
          <h2 class="text-2xl font-semibold tracking-tight mx-auto text-center">
            {{ 'Version ' + installedVersion.version }}
          </h2>
        </template>
        <div class="space-y-4 text-sm">
          <!-- Fatal Errors (embedded) -->
          <UiAlert v-if="fancyLogs.some((l) => l.level === 'Fatal')" variant="danger">
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
                  class="inline-flex items-center gap-2 px-3 py-1.5 rounded-lg bg-danger-600 text-white hover:bg-danger-500 transition"
                  href="./troubleshooting#logs"
                >
                  <i class="fas fa-file-lines" /> {{ $t('index.view_logs') || 'View Logs' }}
                </a>
              </div>
            </div>
          </UiAlert>
          <div v-if="loading" class="text-xs italic flex items-center gap-2">
            <i class="fas fa-spinner animate-spin" /> {{ $t('index.loading_latest') }}
          </div>
          <div v-if="branch || commit" class="flex items-center gap-2 text-xs">
            <span
              v-if="branch"
              class="inline-flex items-center gap-1 px-2 py-0.5 rounded-md bg-black/5 dark:bg-white/10"
            >
              <i class="fas fa-code-branch" /> {{ branch }}
            </span>
            <span
              v-if="commit"
              class="inline-flex items-center gap-1 px-2 py-0.5 rounded-md bg-black/5 dark:bg-white/10 font-mono"
            >
              <i class="fas fa-hashtag" /> {{ commit.substring(0, 7) }}
            </span>
          </div>
          <UiAlert v-if="buildVersionIsDirty" variant="success">
            <template #icon><i class="fas fa-seedling" /></template>
            {{ $t('index.version_dirty') }} 🌇
          </UiAlert>
          <UiAlert v-if="installedVersionNotStable" variant="info">
            <template #icon><i class="fas fa-flask" /></template>
            {{ $t('index.installed_version_not_stable') }}
          </UiAlert>
          <UiAlert
            v-if="compareChecked && !installedVersionNotStable && aheadByCommits > 0"
            variant="success"
          >
            <template #icon><i class="fas fa-forward" /></template>
            {{ $t('index.version_ahead', { ahead: aheadByCommits }) }}
          </UiAlert>
          <UiAlert
            v-if="
              compareChecked &&
              !installedVersionNotStable &&
              aheadByCommits === 0 &&
              behindByCommits > 0
            "
            variant="warning"
          >
            <template #icon><i class="fas fa-clock-rotate-left" /></template>
            {{ $t('index.version_behind', { behind: behindByCommits }) }}
          </UiAlert>
          <UiAlert
            v-if="
              compareChecked &&
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
                  :href="preReleaseRelease?.html_url"
                  target="_blank"
                  >{{ $t('index.download') }}</a
                >
              </div>
              <div
                class="bg-light/5 dark:bg-dark/5 rounded p-3 overflow-auto max-h-64 text-xs font-mono"
              >
                <p class="font-semibold mb-2">{{ preReleaseRelease?.name }}</p>
                <pre class="whitespace-pre-wrap">{{ preReleaseRelease?.body }}</pre>
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
                  :href="githubRelease?.html_url"
                  target="_blank"
                  >{{ $t('index.download') }}</a
                >
              </div>
              <div
                class="bg-light/5 dark:bg-dark/5 rounded p-3 overflow-auto max-h-64 text-xs font-mono"
              >
                <p class="font-semibold mb-2">{{ githubRelease?.name }}</p>
                <pre class="whitespace-pre-wrap">{{ githubRelease?.body }}</pre>
              </div>
            </div>
          </UiAlert>
        </div>
      </UiCard>

      <!-- Resources -->
      <UiCard>
        <template #title>
          <h2 class="text-2xl font-semibold tracking-tight mx-auto text-center">
            {{ $t('resources.title') || 'Resources' }}
          </h2>
        </template>
        <div class="text-xs space-y-2">
          <ResourceCard />
        </div>
      </UiCard>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, computed } from 'vue';
import UiAlert from '@/components/UiAlert.vue';
import UiCard from '@/components/UiCard.vue';
import ResourceCard from '@/ResourceCard.vue';
import SunshineVersion, { GitHubRelease } from '@/sunshine_version';
import { useConfigStore } from '@/stores/config';
import { useAuthStore } from '@/stores/auth';

const installedVersion = ref<SunshineVersion>(new SunshineVersion('0.0.0'));
const githubRelease = ref<GitHubRelease | null>(null);
const preReleaseRelease = ref<GitHubRelease | null>(null);

const githubVersion = computed(() =>
  githubRelease.value
    ? SunshineVersion.fromRelease(githubRelease.value)
    : new SunshineVersion('0.0.0'),
);
const preReleaseVersion = computed(() =>
  preReleaseRelease.value
    ? SunshineVersion.fromRelease(preReleaseRelease.value)
    : new SunshineVersion('0.0.0'),
);
const notifyPreReleases = ref(false);
const loading = ref(true);
const logs = ref('');
const branch = ref('');
const commit = ref('');
const compareInfo = ref<{
  ahead_by: number;
  behind_by: number;
  status: 'identical' | 'ahead' | 'behind' | 'diverged';
} | null>(null);
// Compare info against the latest pre-release tag (if present)
const preCompareInfo = ref<{
  ahead_by: number;
  behind_by: number;
  status: 'identical' | 'ahead' | 'behind' | 'diverged';
} | null>(null);
const compareChecked = ref(false); // true once we've attempted to resolve git distance

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
    const serverVersion = configStore.metadata?.version || cfg.version;
    installedVersion.value = new SunshineVersion(serverVersion || '0.0.0');
    branch.value = cfg.branch || '';
    commit.value = cfg.commit || '';

    // Remote release checks (GitHub)
    try {
      githubRelease.value = await fetch(
        'https://api.github.com/repos/LizardByte/Sunshine/releases/latest',
      ).then((r) => r.json());
    } catch (e) {
      // eslint-disable-next-line no-console
      console.warn('[Dashboard] latest release fetch failed', e);
    }
    try {
      const releases = await fetch(
        'https://api.github.com/repos/LizardByte/Sunshine/releases',
      ).then((r) => r.json());
      const pre = Array.isArray(releases) ? releases.find((r) => r.prerelease) : null;
      if (pre) preReleaseRelease.value = pre;
    } catch (e) {
      // eslint-disable-next-line no-console
      console.warn('[Dashboard] releases list fetch failed', e);
    }

    // Compare if we have enough data. Mark `compareChecked` once we've attempted (success or fail)
    if (commit.value && githubRelease.value?.tag_name) {
      try {
        const baseTag = githubRelease.value.tag_name.startsWith('v')
          ? githubRelease.value.tag_name
          : 'v' + githubRelease.value.tag_name;
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
        // Also attempt compare against pre-release tag if available
        if (preReleaseRelease.value?.tag_name) {
          const preTag = preReleaseRelease.value.tag_name.startsWith('v')
            ? preReleaseRelease.value.tag_name
            : 'v' + preReleaseRelease.value.tag_name;
          const preResp = await fetch(
            `https://api.github.com/repos/LizardByte/Sunshine/compare/${preTag}...${commit.value}`,
          );
          if (preResp.ok) {
            const preCmp = await preResp.json();
            preCompareInfo.value = {
              ahead_by: preCmp.ahead_by,
              behind_by: preCmp.behind_by,
              status: preCmp.status,
            };
          }
        }
      } catch (e) {
        // eslint-disable-next-line no-console
        console.warn('Compare API failed', e);
      } finally {
        compareChecked.value = true;
      }
    } else {
      // Nothing to compare (no commit or no remote version) - treat as checked
      compareChecked.value = true;
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

onMounted(async () => {
  await auth.waitForAuthentication();
  await runVersionChecks();
});

const installedVersionNotStable = computed(() => {
  if (!githubRelease.value || !installedVersion.value) return false;
  // treat non-master branches as pre-release builds automatically
  if (branch.value && branch.value !== 'master') return true;
  return installedVersion.value.isGreaterRelease(githubRelease.value);
});
const stableBuildAvailable = computed(() => {
  if (!githubRelease.value || !installedVersion.value) return false;
  // Don't decide until we've checked git distance
  if (!compareChecked.value) return false;
  // If we have compare info and we're ahead, do not suggest stable update
  if (compareInfo.value && compareInfo.value.ahead_by > 0) return false;
  // If we have compare info and we're exactly equal (ahead_by = behind_by = 0) treat as up-to-date.
  if (compareInfo.value && compareInfo.value.ahead_by === 0 && compareInfo.value.behind_by === 0)
    return false;
  return githubVersion.value.isGreater(installedVersion.value);
});
const preReleaseBuildAvailable = computed(() => {
  if (!preReleaseRelease.value || !githubRelease.value || !installedVersion.value) return false;
  // Only consider pre-release availability after we've confirmed compare status
  if (!compareChecked.value) return false;
  // If our commit is ahead of the pre-release tag, don't suggest the pre-release
  if (preCompareInfo.value && preCompareInfo.value.ahead_by > 0) return false;
  // If exactly equal to the pre-release commit, also do not suggest
  if (
    preCompareInfo.value &&
    preCompareInfo.value.ahead_by === 0 &&
    preCompareInfo.value.behind_by === 0
  )
    return false;
  return (
    preReleaseVersion.value.isGreater(installedVersion.value) &&
    preReleaseVersion.value.isGreater(githubVersion.value)
  );
});
const buildVersionIsDirty = computed(() => {
  if (!installedVersion.value) return false;
  return (
    installedVersion.value.version?.split('.').length === 5 &&
    installedVersion.value.version.indexOf('dirty') !== -1
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
