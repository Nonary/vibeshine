import assert from 'node:assert/strict';
import test from 'node:test';

import { createSteam, findHost, menuLabels, mount, render } from './support/fake-steam.ts';

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

/** Colours of the header icons on screen; more than one means a stale icon was left behind. */
function iconColors(nodes: ReturnType<typeof render>): string[] {
  const colors: string[] = [];
  const walk = (list: ReturnType<typeof render>) => {
    for (const node of list) {
      if (typeof node === 'string') continue;
      if (isIcon(node)) colors.push(node.props.style.color);
      else walk(node.children);
    }
  };
  walk(nodes);
  return colors;
}

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

test('hides its entries while the helper is gone, and shows them when it checks in again', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  steam.advance(14_000);
  assert.equal(menuLabels(steam.openPowerMenu())[0], 'End Stream (Vibeshine)');
  assert.ok(findHost(steam.renderHeader(), isIcon));

  steam.advance(2_000);
  assert.deepEqual(menuLabels(steam.openPowerMenu()), STEAM_POWER_MENU);
  assert.equal(findHost(steam.renderHeader(), isIcon), undefined);

  steam.window.VibeshineBPM.heartbeat();
  assert.equal(menuLabels(steam.openPowerMenu())[0], 'End Stream (Vibeshine)');
  assert.ok(findHost(steam.renderHeader(), isIcon));
});

test('keeps its entries while the helper checks in without status changes', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  for (let i = 0; i < 6; i += 1) {
    steam.advance(5_000);
    steam.window.VibeshineBPM.heartbeat();
  }
  assert.equal(menuLabels(steam.openPowerMenu())[0], 'End Stream (Vibeshine)');
});

test('notices a missing helper when the Power menu opens, even with throttled timers', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  steam.skip(20_000);
  assert.deepEqual(menuLabels(steam.openPowerMenu()), STEAM_POWER_MENU);
});

test('runs one staleness timer per injection and stops it when unpatched', () => {
  const steam = createSteam();
  steam.inject();
  steam.inject();
  assert.equal(steam.timers.size, 1);
  steam.window.VibeshineBPM.unpatch();
  assert.equal(steam.timers.size, 0);
  steam.inject({ checkOnly: true });
  assert.equal(steam.timers.size, 0);
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

test('shows names with $ replacement patterns literally', () => {
  const steam = createSteam();
  steam.inject();
  const name = "Bob$'s $& PC $$";
  steam.window.VibeshineBPM.setStatus({
    ...streaming,
    clients: [{ id: 'webrtc-2', name, kind: 'browser' }],
  });
  assert.deepEqual(menuLabels(steam.openPowerMenu()).slice(0, 2), [
    'End Stream (Vibeshine)',
    `Stop Streaming To ${name} (Vibeshine)`,
  ]);
  const icon = findHost(steam.renderHeader(), isIcon)!;
  assert.equal(icon.props['aria-label'], `Streaming to ${name} with Vibeshine`);
});

test('adds the header icon once when Steam reuses the header children between renders', () => {
  const steam = createSteam({ hoistHeaderItems: true });
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  steam.renderHeader();
  steam.renderHeader();
  assert.deepEqual(headerOrder(steam.renderHeader()), [
    'Search',
    'Internet Settings',
    'Notifications',
    'ICON',
    '4:06 PM',
    'View Account',
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

test('shows the icon in a header on screen as soon as a stream starts', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(idle);
  const header = steam.mountHeader();
  assert.deepEqual(iconColors(header.nodes()), []);
  steam.window.VibeshineBPM.setStatus(streaming);
  assert.deepEqual(iconColors(header.nodes()), ['#5ad45a']);
});

for (const hoistHeaderItems of [false, true]) {
  const shape = hoistHeaderItems ? ', with header items Steam keeps between renders' : '';

  test(`removes the header icon on screen when the controls are turned off${shape}`, () => {
    const steam = createSteam({ hoistHeaderItems });
    steam.inject();
    steam.window.VibeshineBPM.setStatus(streaming);
    const header = steam.mountHeader();
    assert.deepEqual(iconColors(header.nodes()), ['#5ad45a']);
    // The helper unpatches when it stops, as when the controls are turned off.
    steam.window.VibeshineBPM.unpatch();
    assert.deepEqual(iconColors(header.nodes()), []);
    assert.deepEqual(iconColors(header.update()), []);
  });

  test(`a header on screen shows the new injection's icon after the helper restarts${shape}`, () => {
    const steam = createSteam({ hoistHeaderItems });
    steam.inject();
    steam.window.VibeshineBPM.setStatus(streaming);
    const header = steam.mountHeader();
    assert.deepEqual(iconColors(header.nodes()), ['#5ad45a']);

    const report = steam.inject();
    assert.equal(report.features.header_icon, 'ok (already in the header)');
    // The new injection has no status yet, so the old icon must not linger.
    assert.deepEqual(iconColors(header.nodes()), []);
    steam.window.VibeshineBPM.setStatus(paused);
    assert.deepEqual(iconColors(header.nodes()), ['#f0b03c']);
    assert.deepEqual(iconColors(header.update()), ['#f0b03c']);
  });
}

test('replaces an icon a script with another header slot left in the header list', () => {
  const steam = createSteam({ hoistHeaderItems: true });
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  steam.renderHeader();
  // As if the next script came with a different slot.
  steam.window.__vibeshineHeaderSlot.version = 0;
  steam.inject();
  steam.window.VibeshineBPM.setStatus(paused);
  assert.deepEqual(iconColors(steam.renderHeader()), ['#f0b03c']);
});

test('removes the header icon on screen when too many errors switch the script off', () => {
  const steam = createSteam();
  steam.inject({ simulateCrash: ['power-items'], maxErrors: 2 });
  steam.window.VibeshineBPM.setStatus(streaming);
  const header = steam.mountHeader();
  assert.deepEqual(iconColors(header.nodes()), ['#5ad45a']);
  steam.openPowerMenu();
  steam.openPowerMenu();
  assert.equal(steam.window.VibeshineBPM.report().enabled, false);
  assert.deepEqual(iconColors(header.nodes()), []);
});

test('an open Power menu drops its entries when the script switches off', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  steam.window.SteamUIStore.OpenPowerMenu();
  const menu = mount(steam.createdMenus.at(-1)!.element);
  assert.equal(menuLabels(menu.nodes())[0], 'End Stream (Vibeshine)');
  steam.window.VibeshineBPM.unpatch();
  // Only our entries redraw, and the store they read now serves nothing.
  assert.deepEqual(menuLabels(menu.nodes()), STEAM_POWER_MENU);
});

test('an open Vibeshine menu empties when the script switches off', () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  const icon = findHost(steam.renderHeader(), isIcon)!;
  icon.props.onActivate();
  const menu = mount(steam.createdMenus.at(-1)!.element);
  assert.equal(menuLabels(menu.nodes())[2], 'End Stream (Vibeshine)');
  steam.window.VibeshineBPM.unpatch();
  assert.deepEqual(menuLabels(menu.nodes()), ['----', 'Cancel']);
  // An icon still on screen for a moment opens nothing.
  const menus = steam.createdMenus.length;
  icon.props.onActivate();
  assert.equal(steam.createdMenus.length, menus);
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

// In a game the header sits in the Quick Access Menu, a narrow panel over the game's overlay that
// keeps its own menus inside it. A menu opened in the main Big Picture window brings Steam to the
// front and leaves it there once the menu closes.
const gameOverlay = { name: 'Game overlay' };
const quickAccessMenu = { name: 'Quick Access Menu' };
const overlayWindow = { BrowserWindow: { document: { documentElement: { ownerDocument: { defaultView: gameOverlay } } } } };
const iconElement = { ownerDocument: { defaultView: quickAccessMenu } };
const selectIcon = { currentTarget: iconElement, target: { ownerDocument: iconElement.ownerDocument } };

test("opens the Vibeshine menu over the whole game, like Steam's Power menu", () => {
  const steam = createSteam();
  steam.inject();
  steam.window.VibeshineBPM.setStatus(streaming);
  findHost(steam.renderHeader(overlayWindow), isIcon)!.props.onActivate(selectIcon);
  assert.equal(steam.createdMenus.at(-1)!.window, gameOverlay);
});

test("keeps the Vibeshine menu over the game without Steam's window", () => {
  const steam = createSteam();
  const report = steam.inject({ simulateMissing: ['windowContext'] });
  assert.equal(
    report.features.header_menu,
    'ok (menu parts: search; in games it opens inside the Quick Access Menu)',
  );
  steam.window.VibeshineBPM.setStatus(streaming);
  findHost(steam.renderHeader(overlayWindow), isIcon)!.props.onActivate(selectIcon);
  assert.equal(steam.createdMenus.at(-1)!.target, iconElement);
  assert.equal(steam.createdMenus.at(-1)!.window, quickAccessMenu);
});

test("opens Steam's Power menu from the icon when Steam's menu helper is missing", () => {
  const steam = createSteam();
  const report = steam.inject({ simulateMissing: ['showContextMenu'] });
  assert.equal(report.features.header_menu, 'fallback: icon opens the Power menu');
  steam.window.VibeshineBPM.setStatus(streaming);
  findHost(steam.renderHeader(overlayWindow), isIcon)!.props.onActivate(selectIcon);
  assert.equal(steam.calls.openPowerMenu, 1);
  assert.equal(steam.createdMenus.at(-1)!.window, gameOverlay);
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
