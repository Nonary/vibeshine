import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { runInNewContext } from 'node:vm';

const view = readFileSync(new URL('../views/IntegrationsView.vue', import.meta.url), 'utf8');
const start = view.indexOf('function mangoHudSummary(): IntegrationSummary {');
const end = view.indexOf('\nfunction rtssSummary()', start);
assert.ok(start >= 0 && end > start);
const summaryFunction = view.slice(start, end).replace(': IntegrationSummary', '');

function summarize(status: Record<string, unknown>) {
  return runInNewContext(`${summaryFunction}\nmangoHudSummary()`, {
    mangohud: { value: status },
    isLinux: { value: true },
    t: (key: string) => key,
  });
}

test('active global pacing is reported independently of the manual provider and overlay', () => {
  for (const provider of ['auto', 'global', 'mangohud-proton']) {
    const summary = summarize({
      configured_provider: provider,
      enabled: false,
      global_limiter_active: true,
      global_limiter_available: true,
      mangohud_available: false,
    });
    assert.equal(summary.status, '_common.active', provider);
    assert.equal(summary.tone, 'success', provider);
  }
});

test('inactive providers retain missing dependency warnings and ready status', () => {
  for (const [provider, expectedStatus] of [
    ['auto', 'ui.integrations.mangohud.overlayMissing'],
    ['mangohud-proton', 'ui.integrations.mangohud.overlayMissing'],
    ['global', 'ui.integrations.status.notDetected'],
    ['mangohud', 'ui.integrations.status.notDetected'],
  ]) {
    const summary = summarize({
      configured_provider: provider,
      enabled: false,
      global_limiter_active: false,
      global_limiter_available: false,
      mangohud_available: false,
    });
    assert.equal(summary.status, expectedStatus, provider);
    assert.equal(summary.tone, 'warning', provider);
  }
  const ready = summarize({
    configured_provider: 'global',
    enabled: true,
    global_limiter_available: true,
    global_limiter_active: false,
  });
  assert.equal(ready.status, 'ui.integrations.status.ready');
  assert.equal(ready.tone, 'info');
});
