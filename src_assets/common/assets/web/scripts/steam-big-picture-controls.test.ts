import assert from 'node:assert/strict';
import test from 'node:test';

import { createSteam, findHost, menuLabels, render } from './support/fake-steam.ts';

const STEAM_POWER_MENU = [
  'Suspend System',
  'Turn Off System',
  'Restart System',
  '----',
  'Minimize Steam',
  'Exit Big Picture Mode',
  'Exit Steam',
  '----',
  'Cancel',
];

const streaming = {
  version: 1,
  appRunning: true,
  appName: 'Factorio',
  clients: [
    { id: 'A04551F1-C420-292A-7C8B-D3DBB4047207', name: 'Steam Deck', kind: 'moonlight' },
    { id: 'webrtc-1', name: 'Browser', kind: 'browser' },
  ],
  streams: [{ width: 1280, height: 800, fps: 90, codec: 'HEVC', hdr: false }],
  can: { endStream: true, disconnect: true },
  strings: {},
};

const paused = { ...streaming, clients: [], streams: [] };
const idle = { ...streaming, appRunning: false, appName: '', clients: [], streams: [] };

const isIcon = (node: { props: Record<string, any> }) =>
  String(node.props.className ?? '').includes('vibeshine-bpm-indicator');

function headerOrder(nodes: ReturnType<typeof render>): string[] {
  const header = findHost(nodes, (node) => node.props.id === 'header');
  assert.ok(header, 'header rendered');
  return header.children.map((child) =>
    typeof child === 'string'
      ? child
      : isIcon(child)
        ? 'ICON'
        : String(child.props['aria-label'] ?? child.children.join('')),
  );
}

test('injects into a healthy Steam and reports every feature', () => {
  const steam = createSteam();
  const report = steam.inject();
  assert.equal(report.protocol, 1);
  assert.equal(report.react, '19.1.1');
  assert.equal(report.enabled, true);
  assert.deepEqual(report.features, {
    focusable: 'ok',
    header_styles: 'ok',
    power_menu: 'waiting for the Power menu to open',
    header_menu: 'ok (menu parts: search)',
    header_icon: 'ready; appears the next time Big Picture opens',
  });
  assert.equal(typeof steam.window.VibeshineBPM.setStatus, 'function');
  assert.equal('__vibeshineOptions' in steam.window, false);
  assert.deepEqual(steam.messages, []);
});

test('adds End Stream and Stop Streaming entries to the top of the Power menu', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  assert.deepEqual(menuLabels(steam.openPowerMenu()), [
    'End Stream (Vibeshine)',
    'Stop Streaming To Steam Deck (Vibeshine)',
    'Stop Streaming To Browser (Vibeshine)',
    '----',
    ...STEAM_POWER_MENU,
  ]);
  // Learning Steam's menu parts from the Power menu also upgrades the header menu.
  assert.deepEqual(steam.messages, [
    { type: 'feature', name: 'header_menu', value: 'ok (menu parts: learned)' },
    { type: 'feature', name: 'power_menu', value: 'ok (menu parts: learned)' },
  ]);
});

test('leaves the Power menu untouched while nothing is streaming', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(idle);
  assert.deepEqual(menuLabels(steam.openPowerMenu()), STEAM_POWER_MENU);
});

test('selecting the entries sends end_stream and disconnect actions', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  const menu = steam.openPowerMenu();
  steam.messages.length = 0;
  for (const label of ['End Stream (Vibeshine)', 'Stop Streaming To Steam Deck (Vibeshine)']) {
    const item = findHost(menu, (node) => node.children.join('') === label);
    item?.props.onSelected();
  }
  assert.deepEqual(steam.messages, [
    { type: 'action', action: 'end_stream' },
    { type: 'action', action: 'disconnect', client: 'A04551F1-C420-292A-7C8B-D3DBB4047207' },
  ]);
});

test('offers only the actions the host allows', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus({
    ...streaming,
    can: { endStream: true, disconnect: false },
  });
  assert.deepEqual(menuLabels(steam.openPowerMenu()).slice(0, 2), [
    'End Stream (Vibeshine)',
    '----',
  ]);
});

test('uses the labels from the status payload', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus({
    ...streaming,
    clients: [streaming.clients[0]],
    strings: { endStream: 'Stream beenden', stopStreamingTo: 'Streaming zu %1$s beenden' },
  });
  assert.deepEqual(menuLabels(steam.openPowerMenu()).slice(0, 2), [
    'Stream beenden',
    'Streaming zu Steam Deck beenden',
  ]);
});

test('places the header icon left of the clock', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  assert.deepEqual(headerOrder(steam.renderHeader()), [
    'Search',
    'Internet Settings',
    'Notifications',
    'ICON',
    '4:06 PM',
    'View Account',
  ]);
  const icon = findHost(steam.renderHeader(), isIcon)!;
  assert.equal(icon.props['aria-label'], 'Streaming to Steam Deck, Browser with Vibeshine');
  assert.equal(icon.props.style.color, '#5ad45a');
  assert.equal(steam.messages.at(-1).value, 'ok (left of the clock)');
});

test('shows an amber icon while the game runs with nobody connected, and none when idle', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(paused);
  const icon = findHost(steam.renderHeader(), isIcon)!;
  assert.equal(icon.props.style.color, '#f0b03c');
  assert.equal(
    icon.props['aria-label'],
    'Factorio is running on Vibeshine with no client connected',
  );
  steam.window.VibeshineBPM.setStatus(idle);
  assert.equal(findHost(steam.renderHeader(), isIcon), undefined);
});

test('puts the header icon at the end when the clock is not recognised', () => {
  const steam = createSteam({
    clockSource: 'function gi(){return(0,e.jsx)(se,{className:Ne().Time})}',
  });
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  assert.deepEqual(headerOrder(steam.renderHeader()).slice(-3), [
    '4:06 PM',
    'View Account',
    'ICON',
  ]);
  assert.equal(steam.messages.at(-1).value, 'ok (end of header; clock not recognised)');
});

test('opens the Vibeshine menu from the header icon', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  findHost(steam.renderHeader(), isIcon)!.props.onActivate();
  const menu = render(steam.createdMenus.at(-1)!.element);
  assert.equal(
    findHost(menu, (node) => node.props.className === 'contextMenu')!.props['aria-label'],
    'Vibeshine · streaming to Steam Deck, Browser',
  );
  assert.deepEqual(menuLabels(menu), [
    'Factorio · 1280×800 @ 90 fps · HEVC',
    '----',
    'End Stream (Vibeshine)',
    'Stop Streaming To Steam Deck (Vibeshine)',
    'Stop Streaming To Browser (Vibeshine)',
    '----',
    'Cancel',
  ]);
  assert.equal(steam.calls.openPowerMenu, 0);
});

test("opens Steam's Power menu from the icon when Steam's menu helper is missing", () => {
  const steam = createSteam();
  const report = steam.inject({ simulateMissing: ['showContextMenu'] });
  assert.equal(report.features.header_menu, 'fallback: icon opens the Power menu');
  steam.window.VibeshineBPM.setStatus(streaming);
  findHost(steam.renderHeader(), isIcon)!.props.onActivate();
  assert.equal(steam.calls.openPowerMenu, 1);
  assert.equal(menuLabels(render(steam.createdMenus.at(-1)!.element))[0], 'End Stream (Vibeshine)');
});

test("falls back to a plain clickable icon without Steam's focus component and styles", () => {
  const steam = createSteam();
  const report = steam.inject({ simulateMissing: ['focusable', 'headerCss'] });
  assert.equal(report.features.focusable, 'fallback: plain clickable icon (no controller focus)');
  assert.equal(report.features.header_styles, 'fallback: inline styles');
  steam.window.VibeshineBPM.setStatus(streaming);
  const icon = findHost(steam.renderHeader(), isIcon)!;
  assert.equal(icon.props.className, 'vibeshine-bpm-indicator');
  assert.equal(typeof icon.props.onClick, 'function');
  assert.equal(icon.props.style.cursor, 'pointer');
});

test('leaves Steam untouched when its components cannot be found', () => {
  const steam = createSteam();
  const report = steam.inject({
    simulateMissing: [
      'contextMenuHook',
      'header',
      'menu',
      'menuItem',
      'menuSeparator',
      'focusable',
      'headerCss',
      'showContextMenu',
    ],
  });
  assert.deepEqual(report.features, {
    focusable: 'fallback: plain clickable icon (no controller focus)',
    header_styles: 'fallback: inline styles',
    power_menu: 'off: context menu manager not found',
    header_menu: 'fallback: icon opens the Power menu',
    header_icon: 'off: header component not found',
  });
  assert.equal(
    steam.components.CContextMenuManager.prototype.CreateContextMenuInstance,
    steam.originals.createContextMenuInstance,
  );
  assert.equal(steam.components.HeaderBar.type, steam.originals.headerType);
  steam.window.VibeshineBPM.setStatus(streaming);
  assert.deepEqual(menuLabels(steam.openPowerMenu()), STEAM_POWER_MENU);
});

test('throws before patching anything when React cannot be found', () => {
  const steam = createSteam();
  assert.throws(() => steam.inject({ simulateMissing: ['react'] }), /React not found/);
  assert.equal(
    steam.components.CContextMenuManager.prototype.CreateContextMenuInstance,
    steam.originals.createContextMenuInstance,
  );
  assert.equal(steam.components.HeaderBar.type, steam.originals.headerType);
  assert.equal(steam.window.VibeshineBPM, undefined);
});

test('check mode reports without patching or messaging', () => {
  const steam = createSteam();
  const report = steam.inject({ checkOnly: true });
  assert.equal(report.features.power_menu, 'ready');
  assert.equal(report.features.header_icon, 'ready');
  assert.equal(steam.window.VibeshineBPM, undefined);
  assert.equal(
    steam.components.CContextMenuManager.prototype.CreateContextMenuInstance,
    steam.originals.createContextMenuInstance,
  );
  assert.equal(steam.components.HeaderBar.type, steam.originals.headerType);
  assert.deepEqual(steam.messages, []);
});

test("keeps Steam's source text visible through its wrappers", () => {
  const steam = createSteam();
  steam.inject();
  const hooked = steam.components.CContextMenuManager.prototype.CreateContextMenuInstance;
  assert.notEqual(hooked, steam.originals.createContextMenuInstance);
  assert.equal(String(hooked), String(steam.originals.createContextMenuInstance));
  assert.notEqual(steam.components.HeaderBar.type, steam.originals.headerType);
  assert.equal(String(steam.components.HeaderBar.type), String(steam.originals.headerType));
  // A check run while injected still finds everything.
  const check = steam.inject({ checkOnly: true });
  assert.equal(check.features.power_menu, 'ready');
  assert.equal(check.features.header_icon, 'ready');
});

test('replaces a previous injection instead of stacking', () => {
  const steam = createSteam();
  steam.inject();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  assert.equal(headerOrder(steam.renderHeader()).filter((entry) => entry === 'ICON').length, 1);
  assert.equal(
    menuLabels(steam.openPowerMenu()).filter((entry) => entry === 'End Stream (Vibeshine)').length,
    1,
  );
});

test("never removes another mod's patch layered on top", () => {
  const steam = createSteam();
  steam.inject();
  const ours = steam.components.HeaderBar.type;
  const otherMod = (...args: unknown[]) => ours(...args);
  steam.components.HeaderBar.type = otherMod;
  steam.window.VibeshineBPM.unpatch();
  assert.equal(steam.components.HeaderBar.type, otherMod);
  // Our wrapper is still in the chain but now passes Steam's header through unchanged.
  assert.equal(findHost(steam.renderHeader(), isIcon), undefined);
  assert.equal(steam.window.VibeshineBPM, undefined);
});

test("contains a crash in its Power menu entries and keeps Steam's menu", () => {
  const steam = createSteam();
  steam.inject({ simulateCrash: ['power-items'] });
  steam.window.VibeshineBPM.setStatus(streaming);
  assert.deepEqual(menuLabels(steam.openPowerMenu()), STEAM_POWER_MENU);
  assert.deepEqual(steam.messages.at(-1), {
    type: 'error',
    where: 'power-items',
    message: 'simulated crash in power-items',
  });
});

test('contains a crash in the header icon and keeps the header', () => {
  const steam = createSteam();
  steam.inject({ simulateCrash: ['header-icon'] });
  steam.window.VibeshineBPM.setStatus(streaming);
  assert.deepEqual(headerOrder(steam.renderHeader()), [
    'Search',
    'Internet Settings',
    'Notifications',
    '4:06 PM',
    'View Account',
  ]);
  assert.equal(steam.messages.at(-1).where, 'header-icon');
});

test('switches itself off and unhooks after too many errors', () => {
  const steam = createSteam();
  steam.inject({ simulateCrash: ['power-items'], maxErrors: 2 });
  steam.window.VibeshineBPM.setStatus(streaming);
  steam.openPowerMenu();
  steam.openPowerMenu();
  assert.deepEqual(steam.messages.at(-1), { type: 'disabled', reason: '2 errors' });
  assert.equal(
    steam.components.CContextMenuManager.prototype.CreateContextMenuInstance,
    steam.originals.createContextMenuInstance,
  );
  assert.equal(steam.components.HeaderBar.type, steam.originals.headerType);
  assert.equal(steam.window.VibeshineBPM.report().enabled, false);
});

test('respects per-feature switches', () => {
  const steam = createSteam();
  const report = steam.inject({ features: { power_menu: false, header_icon: true } });
  assert.equal(report.features.power_menu, 'off: turned off in Vibeshine');
  assert.equal(
    steam.components.CContextMenuManager.prototype.CreateContextMenuInstance,
    steam.originals.createContextMenuInstance,
  );
  steam.window.VibeshineBPM.setStatus(streaming);
  assert.ok(findHost(steam.renderHeader(), isIcon));
});
