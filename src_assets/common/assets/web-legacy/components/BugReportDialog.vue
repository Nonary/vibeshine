<script setup lang="ts">
import { computed, onBeforeUnmount, ref, useId } from 'vue';
import { useI18n } from 'vue-i18n';
import { buildReportLinks, downloadReportLogs } from '../utils/bugReport';

const props = defineProps<{ version?: string; platform?: string }>();
const { t } = useI18n();
const titleId = useId();
const dialog = ref<HTMLDialogElement>();
const shareDetails = ref(false);
const exportState = ref<'idle' | 'pending' | 'started' | 'failed'>('idle');
const copyState = ref<'idle' | 'copied' | 'failed'>('idle');
const report = computed(() => buildReportLinks(props.version, props.platform));
const controller = new AbortController();
let trigger: HTMLElement | null = null;

function open(event: MouseEvent) {
  shareDetails.value = false;
  copyState.value = 'idle';
  trigger = event.currentTarget instanceof HTMLElement ? event.currentTarget : null;
  dialog.value?.showModal();
}

function restoreFocus() {
  if (trigger?.isConnected) trigger.focus();
}

async function exportLogs() {
  if (exportState.value === 'pending') return;
  exportState.value = 'pending';
  try {
    await downloadReportLogs(controller.signal);
    exportState.value = 'started';
  } catch {
    exportState.value = 'failed';
  }
}

async function copyDetails() {
  try {
    await navigator.clipboard.writeText(report.value.details);
    copyState.value = 'copied';
  } catch {
    copyState.value = 'failed';
  }
}

onBeforeUnmount(() => controller.abort());
</script>

<template>
  <slot :open="open" />
  <dialog ref="dialog" class="bug-report" :aria-labelledby="titleId" @close="restoreFocus">
    <div class="bug-report__heading">
      <h2 :id="titleId">{{ t('index.report_bug.title') }}</h2>
      <button class="bug-report__action" type="button" autofocus @click="dialog?.close()">
        {{ t('_common.close') }}
      </button>
    </div>
    <section>
      <h3>{{ t('index.report_bug.logs') }}</h3>
      <p>{{ t('index.report_bug.review') }}</p>
      <button
        class="bug-report__action"
        type="button"
        :aria-disabled="exportState === 'pending' || undefined"
        @click="exportLogs"
      >
        {{ t(`index.report_bug.download.${exportState}`) }}
      </button>
      <div role="status">
        <p v-if="exportState === 'started' || exportState === 'failed'">
          {{ t(`index.report_bug.export.${exportState}`) }}
        </p>
      </div>
    </section>
    <section>
      <h3>{{ t('index.report_bug.issue') }}</h3>
      <p>{{ t('index.report_bug.guide') }}</p>
      <template v-if="report.details">
        <label class="bug-report__option">
          <input v-model="shareDetails" type="checkbox" />
          {{ t('index.report_bug.share') }}
        </label>
        <textarea
          :aria-label="t('index.report_bug.details')"
          :value="report.details"
          rows="2"
          readonly
        />
        <button class="bug-report__action" type="button" @click="copyDetails">
          {{ t('index.report_bug.copy') }}
        </button>
        <div role="status">
          <p v-if="copyState !== 'idle'">{{ t(`index.report_bug.clipboard.${copyState}`) }}</p>
        </div>
      </template>
      <a
        class="bug-report__action bug-report__link"
        :href="shareDetails ? report.prefilled : report.bare"
        target="_blank"
        rel="noopener noreferrer"
        >{{ t('index.report_bug.open') }}</a
      >
    </section>
  </dialog>
</template>

<style scoped>
.bug-report {
  color: var(--vs-color-text-primary, rgb(var(--color-dark)));
  background: var(--vs-color-bg-surface, rgb(var(--color-light)));
  border: 1px solid var(--vs-color-border-subtle, currentColor);
  border-radius: var(--vs-radius-card, 0.75rem);
  box-sizing: border-box;
  width: min(34rem, calc(100% - 2rem));
  max-height: calc(100% - 2rem);
  margin: auto;
  overflow: auto;
  padding: clamp(1rem, 4vw, 1.5rem);
  text-align: start;
}
:global(.dark .bug-report) {
  color: var(--vs-color-text-primary, rgb(var(--color-light)));
  background: var(--vs-color-bg-surface, rgb(var(--color-surface)));
  color-scheme: dark;
}
.bug-report::backdrop {
  background: rgb(0 0 0 / 0.6);
}
.bug-report__heading {
  display: flex;
  gap: 0.75rem;
  align-items: center;
  justify-content: space-between;
}
.bug-report h2,
.bug-report h3 {
  margin: 0;
  font-weight: 650;
}
.bug-report h2 {
  font-size: 1.25rem;
}
.bug-report section {
  margin-top: 1rem;
}
.bug-report p {
  margin: 0.4rem 0 0.7rem;
}
.bug-report__option {
  display: flex;
  align-items: baseline;
  gap: 0.5rem;
}
.bug-report__option input {
  appearance: auto;
  flex: none;
}
.bug-report textarea {
  display: block;
  width: 100%;
  margin: 0.4rem 0;
  padding: 0.4rem 0.6rem;
  border: 1px solid var(--vs-color-border-subtle, currentColor);
  border-radius: 0.35rem;
  background: transparent;
  color: inherit;
  resize: vertical;
}
.bug-report__action {
  display: inline-block;
  border: 1px solid var(--vs-color-border-subtle, currentColor);
  border-radius: 0.35rem;
  padding: 0.4rem 0.7rem;
  background: transparent;
  color: inherit;
  font: inherit;
  cursor: pointer;
}
.bug-report__action:hover,
.bug-report__action:focus-visible {
  background: var(--vs-color-bg-surface-hover, rgb(128 128 128 / 0.15));
}
.bug-report__action[aria-disabled='true'] {
  cursor: progress;
  opacity: 0.7;
}
.bug-report__link {
  margin-top: 0.5rem;
  text-decoration: underline;
}
</style>
