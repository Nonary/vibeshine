import assert from 'node:assert/strict';
import test from 'node:test';

import {
  applyDummyPlugVsyncChange,
  dummyPlugVsyncState,
  isWindowsHost,
  windowsDisplayHealth,
  showWindowsDisplayStatus,
} from '../utils/displayHealth.ts';

test('Windows driver health accepts the runtime status contract without false readiness', () => {
  const statuses = [
    [0, 'ready'],
    ['ready', 'ready'],
    [-1, 'failed'],
    [-2, 'version_incompatible'],
    [-3, 'watchdog_failed'],
    ['0', 'ready'],
    ['-2', 'version_incompatible'],
    [1, 'unknown'],
    ['not-a-status', 'unknown'],
  ] as const;

  for (const [status, expected] of statuses) {
    const health = windowsDisplayHealth({
      platform: 'windows',
      virtual_display_driver: { active: 'vibeshine', configured: 'vibeshine', status },
    });
    assert.equal(health.state, expected);
    assert.equal(health.activeDriver, 'vibeshine');
  }

  const missing = windowsDisplayHealth({ platform: 'windows' });
  assert.equal(missing.state, 'unknown');
  assert.equal(missing.activeDriver, undefined);
  assert.equal(
    windowsDisplayHealth({
      platform: 'windows',
      virtual_display: { ready: true },
    }).state,
    'unknown',
  );
  assert.equal(
    windowsDisplayHealth({
      platform: 'windows',
      virtual_display_driver: {
        configured: 'sudovda',
        status: 1,
        active: null,
        initialization_attempted: false,
      },
    }).state,
    'uninitialized',
  );
  assert.equal(
    windowsDisplayHealth({
      platform: 'windows',
      virtual_display_driver: { configured: 'vibeshine', status_code: -3, active: 'vibeshine' },
    }).state,
    'watchdog_failed',
  );
});

test('driver health identifies the observed driver independently of a draft selection', () => {
  const health = windowsDisplayHealth({
    platform: 'windows',
    virtual_display_driver: {
      active: 'sudovda',
      configured: 'vibeshine',
      status: 'ready',
    },
  });
  assert.equal(health.activeDriver, 'sudovda');
  assert.equal(health.configuredDriver, 'vibeshine');
  assert.equal(health.state, 'ready');
});

test('driver status is Windows-only and Linux metadata remains absent', () => {
  assert.equal(isWindowsHost({ platform: 'windows' }), true);
  assert.equal(isWindowsHost({ platform: 'linux' }), false);
  assert.equal(isWindowsHost({ platform: 'linux', virtual_display_driver: { status: 0 } }), false);
});

test('dummy-plug HDR forces VSYNC off while enabled and preserves it after disabling', () => {
  assert.deepEqual(dummyPlugVsyncState('true', 'false'), {
    active: true,
    forced: true,
    locked: true,
    effectiveVsyncDisabled: true,
  });
  assert.deepEqual(dummyPlugVsyncState(false, 'true'), {
    active: false,
    forced: false,
    locked: false,
    effectiveVsyncDisabled: true,
  });

  const enabled = applyDummyPlugVsyncChange(
    { dd_wa_dummy_plug_hdr10: false, frame_limiter_disable_vsync: false },
    'on',
  );
  assert.deepEqual(enabled, {
    dd_wa_dummy_plug_hdr10: true,
    frame_limiter_disable_vsync: true,
  });
  assert.deepEqual(applyDummyPlugVsyncChange(enabled, false), {
    dd_wa_dummy_plug_hdr10: false,
    frame_limiter_disable_vsync: true,
  });
});

test('inconsistent persisted dummy-plug config stays clean while showing its effective state', () => {
  const loaded = {
    dd_wa_dummy_plug_hdr10: 'enabled',
    frame_limiter_disable_vsync: '0',
    unrelated_setting: 'preserve',
  };
  const effective = dummyPlugVsyncState(
    loaded.dd_wa_dummy_plug_hdr10,
    loaded.frame_limiter_disable_vsync,
  );
  assert.equal(effective.active, true);
  assert.equal(effective.forced, true);
  assert.equal(effective.effectiveVsyncDisabled, true);
  assert.equal(loaded.frame_limiter_disable_vsync, '0');
  assert.equal(loaded.unrelated_setting, 'preserve');
});

test('disabling dummy-plug HDR does not erase an explicit VSYNC override', () => {
  const draft = { dd_wa_dummy_plug_hdr10: false, frame_limiter_disable_vsync: false };
  const enabled = applyDummyPlugVsyncChange(draft, true);
  const disabled = applyDummyPlugVsyncChange(enabled, false);
  assert.equal(disabled.frame_limiter_disable_vsync, true);
});

test('cold startup is unobserved, not ready, while real failures remain failures without an active driver', () => {
  const metadata = {
    platform: 'windows',
    virtual_display_driver: {
      configured: 'vibeshine',
      active: null,
      status: 'unknown',
      status_code: 1,
      initialization_attempted: false,
    },
  };
  assert.equal(windowsDisplayHealth(metadata).state, 'uninitialized');
  assert.equal(windowsDisplayHealth(metadata).activeDriver, undefined);
  for (const status of ['failed', 'version_incompatible', 'watchdog_failed'] as const) {
    assert.equal(
      windowsDisplayHealth({
        ...metadata,
        virtual_display_driver: { ...metadata.virtual_display_driver, status },
      }).state,
      status,
    );
  }
});

const uninitializedMetadata = {
  platform: 'windows',
  virtual_display_driver: {
    configured: 'vibeshine',
    active: null,
    status: 'unknown',
    status_code: 1,
    initialization_attempted: false,
  },
};

test('only an uninitialized non-error Windows driver panel is hidden', () => {
  assert.equal(showWindowsDisplayStatus(uninitializedMetadata), false);
  assert.equal(showWindowsDisplayStatus(uninitializedMetadata, true), true);
  assert.equal(
    showWindowsDisplayStatus(uninitializedMetadata, false, 'Metadata request failed'),
    true,
  );
  for (const status of [
    'ready',
    'failed',
    'version_incompatible',
    'watchdog_failed',
    'unexpected',
    42,
  ]) {
    const metadata = {
      ...uninitializedMetadata,
      virtual_display_driver: { configured: 'vibeshine', active: null, status },
    };
    assert.equal(showWindowsDisplayStatus(metadata), true, String(status));
  }
  assert.equal(showWindowsDisplayStatus({ platform: 'windows' }), true);
  assert.equal(
    showWindowsDisplayStatus({ ...uninitializedMetadata, platform: 'linux' }, true, 'error'),
    false,
  );
});

test('unexpected or missing driver data is not silently treated as deferred startup', () => {
  for (const status of ['unexpected', undefined, 'unknown']) {
    const metadata = {
      ...uninitializedMetadata,
      virtual_display_driver: { configured: 'vibeshine', active: null, status },
    };
    assert.equal(windowsDisplayHealth(metadata).state, 'unknown');
    assert.equal(showWindowsDisplayStatus(metadata), true);
  }
  const malformed = {
    ...uninitializedMetadata,
    virtual_display_driver: {
      ...uninitializedMetadata.virtual_display_driver,
      status: 'unexpected',
    },
  };
  assert.equal(showWindowsDisplayStatus(malformed), true);
});

test('failed recovery UNKNOWN metadata stays visible after an actual initialization attempt', () => {
  for (const initialization_attempted of [true, undefined, 'false']) {
    const metadata = {
      ...uninitializedMetadata,
      virtual_display_driver: {
        ...uninitializedMetadata.virtual_display_driver,
        initialization_attempted,
      },
    };
    assert.equal(windowsDisplayHealth(metadata).state, 'unknown');
    assert.equal(showWindowsDisplayStatus(metadata), true);
  }
  const explicitUnknown = {
    ...uninitializedMetadata,
    virtual_display_driver: {
      ...uninitializedMetadata.virtual_display_driver,
      status: 'uninitialized',
      initialization_attempted: true,
    },
  };
  assert.equal(showWindowsDisplayStatus(explicitUnknown), true);
});
