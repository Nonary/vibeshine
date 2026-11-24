// Vibeshine stream controls for Steam Big Picture.
//
// Injected into Steam's shared JS context (SharedJSContext) by tools/sunshine_steam_ui_helper.exe. See
// docs/steam-big-picture-controls.md, and src/steam_big_picture_controls_protocol.h for the message formats.
//
// - Power menu: "End Stream" and "Stop Streaming To <client>" entries at the top.
// - Header: a status icon left of the clock that opens a Vibeshine menu with the same actions.
//
// Hardening rules, so a Steam update degrades this instead of breaking Steam:
// - Each feature is set up and runs on its own; one failing never disables another.
// - Everything we render sits inside our own error boundary, so a crash only removes our items.
// - Patches run after Steam has rendered, contain no hooks, and pass Steam's output through untouched
//   when it doesn't look as expected.
// - Steam code is found by stable names (text IDs, prop names, CSS keys, method names), never by
//   minified identifiers or module ids. Menu components are learned from Steam's own Power menu.
//   Steam ships its UI as webpack chunks, or (its beta from November 2026) as native ES modules, whose
//   builds don't export the header and spell out class names instead of CSS keys; there the header,
//   its clock and its item component are learned from the React tree of Big Picture on screen.
// - Patches check an on/off flag, so switching off never unwinds another mod's patches. After too many
//   errors the script switches itself off. Switching off redraws what we rendered, so none of it stays
//   on screen.
//
// Status arrives via window.VibeshineBPM.setStatus(); actions and reports go back through
// window.__vibeshineBridge, a CDP binding the helper installs. The helper also calls heartbeat() every
// few seconds: after 15 s without hearing from it, the entries hide until it is back.
//
// Options (window.__vibeshineOptions, read once at injection):
//   checkOnly        find everything, patch nothing, return the report
//   features         {power_menu, header_icon}; false switches that feature off
//   simulateMissing  lookups to treat as not found (fault injection)
//   simulateCrash    our components to make throw while rendering (fault injection)
//   maxErrors        errors allowed before the script switches itself off (default 8)
//   loadModule       tests only: loads an ES module by URL in place of import()
//
// On webpack builds the injection returns its report; on ES-module builds, which can only be read
// asynchronously, a promise of it.
(function main(preloaded) {
  const TAG = '[Vibeshine]';
  const PROTOCOL = 1;
  const VERSION = 1;
  // Read once: an ES-module build starts over with the options and the modules it read.
  const options = preloaded?.options ?? window.__vibeshineOptions ?? {};
  delete window.__vibeshineOptions;
  const MAX_ERRORS = options.maxErrors ?? 8;
  const simulateMissing = new Set(options.simulateMissing ?? []);
  const simulateCrash = new Set(options.simulateCrash ?? []);
  const featureEnabled = (name) => options.features?.[name] !== false;

  // Remove a previous injection first, so the searches below see Steam's own code.
  if (!options.checkOnly) window.VibeshineBPM?.unpatch?.();

  // ---------------------------------------------------------------------------------------------
  // State, error budget, reporting
  // ---------------------------------------------------------------------------------------------
  const state = { enabled: true, errors: 0, started: false };
  const features = {};
  const cleanups = [];

  function sendToBridge(message) {
    if (options.checkOnly) return;
    try {
      if (typeof window.__vibeshineBridge === 'function') window.__vibeshineBridge(JSON.stringify(message));
    } catch {}
  }

  function setFeature(name, value) {
    if (features[name] === value) return;
    features[name] = value;
    // During setup the injection's return value carries the full report; afterwards report changes live.
    if (state.started) sendToBridge({ type: 'feature', name, value });
  }

  function disable(reason, { notify: announce = true } = {}) {
    if (!state.enabled) return;
    state.enabled = false;
    for (const undo of cleanups.splice(0)) {
      try {
        undo();
      } catch {}
    }
    // Nothing else redraws what we rendered, so it would stay on screen, frozen, until Big Picture
    // reopens. The store now reads as empty, so it redraws as nothing.
    notify();
    if (announce) sendToBridge({ type: 'disabled', reason });
  }

  function recordError(where, error) {
    state.errors++;
    console.error(TAG, where, error);
    sendToBridge({ type: 'error', where, message: String(error?.message ?? error).slice(0, 300) });
    if (state.errors >= MAX_ERRORS) disable(`${state.errors} errors`);
  }

  // Our wrappers look like the original from outside (same props, same source text), so finders in
  // other mods, and in our own next injection, still recognise the Steam function underneath.
  function disguise(wrapper, original) {
    Object.assign(wrapper, original);
    Object.defineProperty(wrapper, 'toString', { value: () => original.toString(), configurable: true });
    return wrapper;
  }

  const safeHandler = (where, fn) =>
    function (...args) {
      try {
        return fn.apply(this, args);
      } catch (e) {
        recordError(where, e);
      }
    };

  // ---------------------------------------------------------------------------------------------
  // Finding Steam's code. Modules are matched on stable names in their source; loading a webpack
  // module runs it, so filters are tight.
  // - Webpack builds: pushing a fake chunk into the webpack chunk list hands us `require`.
  // - ES-module builds: Steam's own module scripts and the chunks they loaded. Importing a chunk
  //   again hands back the module already running, so nothing runs twice. A chunk holds many
  //   modules, so there the source test only narrows the search and the export test decides.
  // ---------------------------------------------------------------------------------------------
  const srcOf = (v) => {
    try {
      return (typeof v === 'function' ? v : v?.type ?? v?.render)?.toString() ?? '';
    } catch {
      return '';
    }
  };
  const has = (text, ...needles) => needles.every((n) => text.includes(n));
  const isReact = (m) => typeof m?.createElement === 'function' && typeof m.useSyncExternalStore === 'function' && typeof m.Component === 'function' && typeof m.version === 'string';

  function webpackModules() {
    for (const key of Object.keys(window).filter((k) => k.startsWith('webpackChunk'))) {
      let found;
      try {
        window[key].push([[Symbol('vibeshine-bpm')], {}, (r) => { found = r; }]);
      } catch {}
      if (!found?.m) continue;
      const entries = [];
      for (const [id, factory] of Object.entries(found.m)) {
        try {
          entries.push({ source: factory.toString(), load: () => found(id) });
        } catch {}
      }
      return { kind: 'webpack', entries, react: null };
    }
    return null;
  }

  const hasEsModules = () => !!window.document?.querySelector?.('script[type="module"][src]');

  async function esModules() {
    const load = options.loadModule ?? ((url) => import(url));
    const origin = `${window.location.origin}/`;
    const fetched = new Set((window.performance?.getEntriesByType?.('resource') ?? []).map((e) => e.name));
    const queue = [...window.document.querySelectorAll('script[type="module"][src], link[rel="modulepreload"][href]')].map((el) => el.src || el.href);
    const seen = new Set();
    const chunks = [];
    // Steam's own module scripts, what they import, and chunks they loaded later (Steam fetched
    // those); a chunk it never loaded stays unloaded.
    // Kept in the order they were found, so lookups pick the same export every time.
    while (queue.length) {
      const batch = [...new Set(queue.splice(0))].filter((url) => url.startsWith(origin) && !seen.has(url));
      batch.forEach((url) => seen.add(url));
      const sources = await Promise.all(batch.map((url) => window.fetch(url).then((r) => r.text(), () => null).catch(() => null)));
      batch.forEach((url, i) => {
        const source = sources[i];
        if (typeof source !== 'string') return;
        chunks.push({ url, source });
        // A static import is loaded with its importer; any other chunk named here only if fetched.
        for (const [, staticImport, , spec] of source.matchAll(/(\b(?:from|import)\s*)?(["'`])(\.{1,2}\/[^"'`\s]+?\.m?js(?:\?[^"'`\s]*)?)\2/g)) {
          const next = new window.URL(spec, url).href;
          if (!seen.has(next) && (staticImport || fetched.has(next))) queue.push(next);
        }
      });
    }
    const loaded = await Promise.all(chunks.map((chunk) => Promise.resolve().then(() => load(chunk.url)).catch(() => null)));
    const entries = chunks.flatMap((chunk, i) => (loaded[i] ? [{ source: chunk.source, load: () => loaded[i] }] : []));
    if (!entries.length) return null;
    return { kind: 'esm', entries, react: esReact(entries) };
  }

  // These builds don't export React itself, only its CommonJS loader, which hands back the React
  // module Steam already loaded. Its name, or failing that its shape in the chunk that defines React.
  function esReact(entries) {
    const lazyLoader = (v) => typeof v === 'function' && v.length === 0 && /^\(\)\s*=>\s*\(\w+\s*\|\|/.test(srcOf(v));
    for (const { source, load } of entries) {
      const name = source.match(/export\s*\{[^}]*\brequire_react\s+as\s+([\w$]+)/)?.[1];
      const loader = name && load()[name];
      try {
        const m = typeof loader === 'function' ? loader() : null;
        if (isReact(m)) return m;
      } catch {}
    }
    for (const { source, load } of entries) {
      if (!has(source, '.useSyncExternalStore=function', 'react.transitional.element')) continue;
      for (const loader of Object.values(load()).filter(lazyLoader)) {
        try {
          const m = loader();
          if (isReact(m)) return m;
        } catch {}
      }
    }
    return null;
  }

  // Nothing is patched before these checks, so throwing here leaves Steam untouched; the helper retries.
  const modules = preloaded?.modules ?? webpackModules();
  if (!modules) {
    if (!hasEsModules()) throw new Error(`${TAG} Steam's modules not found`);
    return esModules().then((found) => {
      if (!found) throw new Error(`${TAG} Steam's ES modules could not be read`);
      return main({ options, modules: found });
    });
  }

  function findExport(name, srcTest, exportTest) {
    if (simulateMissing.has(name)) return null;
    for (const { source, load } of modules.entries) {
      if (!srcTest(source)) continue;
      let mod;
      try {
        mod = load();
      } catch {
        continue;
      }
      if (mod == null) continue;
      for (const value of [mod, ...Object.values(mod)]) {
        try {
          if (exportTest(value)) return value;
        } catch {}
      }
    }
    return null;
  }

  const React =
    (simulateMissing.has('react') ? null : modules.react) ??
    findExport('react', (s) => has(s, 'useSyncExternalStore', 'createElement', 'forwardRef') && /\.version\s*=\s*"\d+\.\d+/.test(s), isReact);
  if (!React) throw new Error(`${TAG} React not found`);
  const esm = modules.kind === 'esm';
  const h = React.createElement;
  const MEMO = Symbol.for('react.memo');
  const FORWARD_REF = Symbol.for('react.forward_ref');
  const CONTEXT = Symbol.for('react.context');

  // Steam UI building blocks. Each is optional: features fall back when one is missing.
  const inMenuModule = (s) => has(s, 'onSelected', 'contextMenuItem');
  // ES-module builds quote strings with backticks.
  const isSeparatorSource = (src) => /["'`]separator["'`]/.test(src);
  const isMenuItemLike = (v) => typeof v === 'function' && v.prototype?.isReactComponent && srcOf(v).includes('onSelected');
  const parts = {
    Menu: findExport('menu', inMenuModule, (v) => typeof v === 'function' && has(srcOf(v), 'labelId', 'useId')),
    // Other components mention onSelected too; Steam's menu item also names its class. A chunk of an
    // ES-module build holds many of those components, so there only that test is safe.
    MenuItem:
      findExport('menuItem', inMenuModule, (v) => isMenuItemLike(v) && srcOf(v).includes('contextMenuItem')) ??
      (esm ? null : findExport('menuItem', inMenuModule, isMenuItemLike)),
    MenuSeparator: findExport('menuSeparator', inMenuModule, (v) => typeof v === 'function' && !v.prototype?.isReactComponent && isSeparatorSource(srcOf(v))),
    source: 'search',
  };
  const ui = {
    Focusable: findExport(
      'focusable',
      (s) => has(s, '"flow-children"', 'onActivate', 'focusWithinClassName'),
      (v) => has(srcOf(v), '"flow-children"', 'onActivate', 'onCancel', 'focusClassName', 'focusWithinClassName'),
    ),
    headerCss: findExport(
      'headerCss',
      (s) => has(s, 'HeaderItem:', 'Clickable:', 'Clock:'),
      (m) => typeof m?.HeaderItem === 'string' && typeof m.Clickable === 'string',
    ),
    showContextMenu: findExport(
      'showContextMenu',
      (s) => s.includes('GetContextMenuManagerFromWindow'),
      (v) => typeof v === 'function' && has(srcOf(v), 'GetContextMenuManagerFromWindow', 'CreateContextMenuInstance'),
    ),
    // The Steam window a component is drawn for: in a game, the game's overlay, even for the Quick
    // Access Menu drawn over it. Its value has the window as BrowserWindow. A chunk of an ES-module
    // build holds many contexts, so there it is learned from the screen instead.
    windowContext: esm
      ? null
      : findExport(
          'windowContext',
          (s) => has(s, 'createContext', 'VirtualKeyboardManager', '.instance'),
          (v) => v?.$$typeof === CONTEXT,
        ),
    // Learned from the screen on ES-module builds (see learnFromScreen): Steam's own header item
    // component, which spells out the class names CSS keys used to give, and the clock.
    headerItem: null,
    clock: null,
  };
  const ContextMenuManager = findExport(
    'contextMenuHook',
    (s) => s.includes('CreateContextMenuInstance'),
    (v) => typeof v === 'function' && typeof v.prototype?.CreateContextMenuInstance === 'function',
  );
  const isHeaderBar = (v) => v?.$$typeof === MEMO && typeof v.type === 'function' && has(srcOf(v), 'quickAccessHeader', 'BShowHeader');
  // Not exported by ES-module builds: learned from the screen then (see learnFromScreen).
  let HeaderBar = findExport('header', (s) => has(s, 'quickAccessHeader', 'BShowHeader'), isHeaderBar);
  modules.entries = [];

  // ---------------------------------------------------------------------------------------------
  // Status store and labels
  // ---------------------------------------------------------------------------------------------
  let status = null;
  // The helper checks in every 5 s, with heartbeat() or setStatus(). When it goes quiet it has
  // stopped, crashed or been uninstalled, and nothing would act on our entries, so they hide (the
  // store reads as empty) until it checks in again. It also reads as empty once the script is off.
  const STALE_AFTER_MS = 15000;
  let lastHeard = Date.now();
  let stale = false;
  const listeners = new Set();
  const store = {
    get: () => (stale || !state.enabled ? null : status),
    subscribe(fn) {
      listeners.add(fn);
      return () => listeners.delete(fn);
    },
  };
  function notify() {
    for (const fn of listeners) {
      try {
        fn();
      } catch (e) {
        recordError('store', e);
      }
    }
  }
  function setStatus(next) {
    status = next;
    lastHeard = Date.now();
    stale = false;
    notify();
  }
  function heartbeat() {
    lastHeard = Date.now();
    if (stale) {
      stale = false;
      notify();
    }
  }
  // Runs once a second, and when a context menu opens: Chromium may throttle timers in Steam's
  // hidden shared context, but the Power menu must not offer entries nothing will act on.
  function checkStale() {
    if (stale || Date.now() - lastHeard < STALE_AFTER_MS) return;
    stale = true;
    notify();
  }

  const isActive = (s) => !!(s && (s.appRunning || s.clients?.length));

  // Labels come from the status payload so the host decides wording; these are only fallbacks.
  const DEFAULT_STRINGS = {
    endStream: 'End Stream (Vibeshine)',
    stopStreamingTo: 'Stop Streaming To %1$s (Vibeshine)',
    menuTitleStreaming: 'Vibeshine · streaming to %1$s',
    menuTitlePaused: 'Vibeshine · no client connected',
    iconStreaming: 'Streaming to %1$s with Vibeshine',
    iconPaused: '%1$s is running on Vibeshine with no client connected',
    idle: 'Idle',
  };
  function label(s, key, ...args) {
    const template = typeof s?.strings?.[key] === 'string' && s.strings[key] ? s.strings[key] : DEFAULT_STRINGS[key];
    // A replacer function, so "$&" or "$'" in a client or app name stays literal text.
    return args.reduce((t, arg, i) => t.replace(`%${i + 1}$s`, () => arg), template);
  }

  function describeStream(s) {
    const st = s?.streams?.[0];
    const details = [s?.appName || null];
    if (st) details.push(`${st.width}×${st.height} @ ${st.fps} fps`, [st.codec, st.hdr && 'HDR'].filter(Boolean).join(' '));
    return details.filter(Boolean).join(' · ');
  }

  const clientNames = (s) => (s?.clients ?? []).map((c) => c.name).join(', ');

  // ---------------------------------------------------------------------------------------------
  // Shared React pieces
  // ---------------------------------------------------------------------------------------------
  class SafeBoundary extends React.Component {
    constructor(props) {
      super(props);
      this.state = { failed: false };
    }
    static getDerivedStateFromError() {
      return { failed: true };
    }
    componentDidCatch(error) {
      recordError(this.props.name, error);
    }
    render() {
      return this.state.failed || !state.enabled ? null : this.props.children;
    }
  }
  const safe = (name, element, key) => h(SafeBoundary, { name, key }, element);

  const useStatus = () => React.useSyncExternalStore(store.subscribe, store.get);
  const crashIfSimulated = (name) => {
    if (simulateCrash.has(name)) throw new Error(`simulated crash in ${name}`);
  };

  function actionItems(s, MenuItem) {
    if (!isActive(s)) return [];
    const items = [];
    if (s.can?.endStream !== false) {
      items.push(h(MenuItem, { key: 'vibeshine-end-stream', tone: 'destructive', onSelected: safeHandler('end_stream', () => sendToBridge({ type: 'action', action: 'end_stream' })) }, label(s, 'endStream')));
    }
    if (s.can?.disconnect !== false) {
      for (const c of s.clients ?? []) {
        items.push(
          h(
            MenuItem,
            { key: `vibeshine-disconnect-${c.id}`, onSelected: safeHandler('disconnect', () => sendToBridge({ type: 'action', action: 'disconnect', client: c.id })) },
            label(s, 'stopStreamingTo', c.name),
          ),
        );
      }
    }
    return items;
  }

  // Flatten children arrays and fragments into a list of elements.
  function flatten(children, out = []) {
    for (const child of [].concat(children ?? [])) {
      if (Array.isArray(child)) flatten(child, out);
      else if (React.isValidElement(child) && child.type === React.Fragment) flatten(child.props.children, out);
      else if (React.isValidElement(child)) out.push(child);
    }
    return out;
  }

  // Steam's Power menu shows its own MenuItem / MenuSeparator / Menu, so take them from there.
  function learnMenuParts(menu) {
    const elements = flatten(menu.props.children);
    const counts = new Map();
    for (const el of elements) {
      if (typeof el.props?.onSelected === 'function' && typeof el.props.children === 'string') counts.set(el.type, (counts.get(el.type) ?? 0) + 1);
    }
    const MenuItem = [...counts].sort((a, b) => b[1] - a[1])[0]?.[0];
    const MenuSeparator = elements.find((el) => el.props && Object.keys(el.props).length === 0 && srcOf(el.type).includes('separator'))?.type;
    if (MenuItem && MenuSeparator && parts.source !== 'learned') {
      Object.assign(parts, { Menu: menu.type, MenuItem, MenuSeparator, source: 'learned' });
      updateHeaderMenuFeature();
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Feature: Power menu entries.
  // Steam creates every context menu through CContextMenuManager.CreateContextMenuInstance(element, ...).
  // We watch that call; when the element is the Power menu we swap in a wrapper component that renders
  // Steam's menu and then adds our entries. Nothing is called with fake input and no global is replaced.
  // ---------------------------------------------------------------------------------------------
  function renderFunctionOf(type) {
    if (typeof type === 'function') return type.prototype?.isReactComponent ? null : type;
    if (type?.$$typeof === MEMO) return renderFunctionOf(type.type);
    if (type?.$$typeof === FORWARD_REF) return type.render;
    return null;
  }

  const POWER_MENU_ANCHORS = ['#StopStreamingTo', '#ExitGamepadUI', '#MinimizeSteam', /["'`]#Power["'`]/];
  function isPowerMenu(element) {
    if (!React.isValidElement(element)) return false;
    if (element.props && 'isPowerChord' in element.props) return true;
    const src = srcOf(element.type);
    return POWER_MENU_ANCHORS.filter((a) => (typeof a === 'string' ? src.includes(a) : a.test(src))).length >= 2;
  }

  function PowerMenuItems({ MenuItem, MenuSeparator }) {
    const s = useStatus();
    crashIfSimulated('power-items');
    const items = actionItems(s, MenuItem);
    if (!items.length) return null;
    return h(React.Fragment, null, ...items, h(MenuSeparator, { key: 'vibeshine-sep' }));
  }

  function addPowerItems(menu) {
    if (!React.isValidElement(menu) || menu.props?.children == null) {
      setFeature('power_menu', 'off: Power menu output not recognised');
      return menu;
    }
    learnMenuParts(menu);
    const { MenuItem, MenuSeparator } = parts;
    if (!MenuItem || !MenuSeparator) {
      setFeature('power_menu', 'off: menu items not recognised');
      return menu;
    }
    setFeature('power_menu', `ok (menu parts: ${parts.source})`);
    return React.cloneElement(menu, undefined, safe('power-items', h(PowerMenuItems, { MenuItem, MenuSeparator }), 'vibeshine-power-items'), menu.props.children);
  }

  const powerMenuWrappers = new WeakMap();
  function powerMenuWrapperFor(render) {
    if (!powerMenuWrappers.has(render)) {
      powerMenuWrappers.set(render, function VibeshinePowerMenu(props) {
        const menu = render(props); // Steam's own render; its errors behave exactly as without us
        if (!state.enabled) return menu;
        try {
          return addPowerItems(menu);
        } catch (e) {
          recordError('power-menu', e);
          return menu;
        }
      });
    }
    return powerMenuWrappers.get(render);
  }

  function onContextMenuCreated(element) {
    if (!isPowerMenu(element)) return element;
    const render = renderFunctionOf(element.type);
    if (!render) {
      setFeature('power_menu', 'off: Power menu component type not supported');
      return element;
    }
    return h(powerMenuWrapperFor(render), { ...element.props, key: element.key ?? undefined });
  }

  function setupPowerMenu() {
    if (!featureEnabled('power_menu')) return setFeature('power_menu', 'off: turned off in Vibeshine');
    if (!ContextMenuManager) return setFeature('power_menu', 'off: context menu manager not found');
    if (options.checkOnly) return setFeature('power_menu', 'ready');
    const proto = ContextMenuManager.prototype;
    const original = proto.CreateContextMenuInstance;
    const hooked = disguise(function (element, ...rest) {
      if (state.enabled) {
        try {
          checkStale();
          element = onContextMenuCreated(element);
        } catch (e) {
          recordError('context-menu-hook', e);
        }
      }
      return original.call(this, element, ...rest);
    }, original);
    proto.CreateContextMenuInstance = hooked;
    cleanups.push(() => {
      if (proto.CreateContextMenuInstance === hooked) proto.CreateContextMenuInstance = original;
    });
    setFeature('power_menu', 'waiting for the Power menu to open');
  }

  // ---------------------------------------------------------------------------------------------
  // Feature: header icon and its menu
  // ---------------------------------------------------------------------------------------------
  const svg = (...children) => h('svg', { xmlns: 'http://www.w3.org/2000/svg', viewBox: '0 0 24 24', width: '24px', height: '24px', fill: 'none', stroke: 'currentColor', strokeWidth: 2, strokeLinecap: 'round' }, ...children);
  const streamIcon = () =>
    svg(h('rect', { key: 'screen', x: 2, y: 4, width: 20, height: 13, rx: 2 }), h('path', { key: 'stand', d: 'M8 21h8M12 17v4' }), h('circle', { key: 'dot', cx: 12, cy: 10.5, r: 2.5, fill: 'currentColor', stroke: 'none' }));

  const canShowOwnMenu = () => !!(ui.showContextMenu && parts.Menu && parts.MenuItem && parts.MenuSeparator) && !simulateMissing.has('headerMenu');
  function updateHeaderMenuFeature() {
    const inGames = ui.windowContext ? '' : '; in games it opens inside the Quick Access Menu';
    setFeature('header_menu', canShowOwnMenu() ? `ok (menu parts: ${parts.source}${inGames})` : 'fallback: icon opens the Power menu');
  }

  function HeaderMenuItems({ MenuItem, MenuSeparator }) {
    const s = useStatus();
    crashIfSimulated('header-menu-items');
    // An open menu empties once the helper is gone or the script is off.
    if (!s) return null;
    return h(React.Fragment, null, h(MenuItem, { key: 'info', disabled: true }, describeStream(s) || label(s, 'idle')), h(MenuSeparator, { key: 'sep' }), ...actionItems(s, MenuItem));
  }

  function openPowerMenu(anchor) {
    const open = window.SteamUIStore?.OpenPowerMenu;
    if (typeof open !== 'function') throw new Error('SteamUIStore.OpenPowerMenu not available');
    open.call(window.SteamUIStore, anchor);
  }

  // Steam opens a menu in the window of the element it is given, and in the main Big Picture window
  // for anything else, a window included. In a game that window would come to the front and stay
  // there once the menu closes. The header then sits in the Quick Access Menu over the game, which
  // keeps its own menus inside its narrow panel, so the menu opens on the root of the game's overlay
  // and covers the screen like the Power menu; Steam's VR Power button does the same. Without that
  // window, the icon itself still keeps the menu over the game.
  function menuAnchor(evt, steamWindow) {
    const root = steamWindow?.BrowserWindow?.document?.documentElement;
    return [root, evt?.currentTarget, evt?.target].find((node) => node?.ownerDocument?.defaultView) ?? null;
  }

  const openVibeshineMenu = safeHandler('header-menu', (evt, steamWindow) => {
    if (!state.enabled) return;
    const anchor = menuAnchor(evt, steamWindow);
    if (!canShowOwnMenu()) return openPowerMenu(anchor);
    const s = store.get();
    const title = s?.clients?.length ? label(s, 'menuTitleStreaming', clientNames(s)) : label(s, 'menuTitlePaused');
    const { Menu, MenuItem, MenuSeparator } = parts;
    ui.showContextMenu(h(Menu, { label: title }, safe('header-menu-items', h(HeaderMenuItems, { MenuItem, MenuSeparator }))), anchor);
  });

  const FALLBACK_ICON_STYLE = { display: 'flex', alignItems: 'center', padding: '0 10px', height: '100%', cursor: 'pointer' };
  const NO_WINDOW_CONTEXT = React.createContext(null);
  function HeaderIcon() {
    const s = useStatus();
    const steamWindow = React.useContext(ui.windowContext ?? NO_WINDOW_CONTEXT);
    crashIfSimulated('header-icon');
    if (!state.enabled || !isActive(s)) return null;
    const live = s.clients?.length > 0;
    const text = live ? label(s, 'iconStreaming', clientNames(s)) : label(s, 'iconPaused', s.appName || 'Game');
    const css = ui.headerCss;
    const color = live ? '#5ad45a' : '#f0b03c';
    const open = (evt) => openVibeshineMenu(evt, steamWindow);
    // Steam's own header item draws it like Steam's icons, with their class names and controller focus.
    if (!css && ui.headerItem) return h(ui.headerItem, { className: 'vibeshine-bpm-indicator', role: 'button', title: text, description: text, style: { color }, onActivate: open }, streamIcon());
    const common = {
      className: css ? `${css.HeaderItem} ${css.Clickable} vibeshine-bpm-indicator` : 'vibeshine-bpm-indicator',
      role: 'button',
      'aria-label': text,
      title: text,
      style: { ...(css ? null : FALLBACK_ICON_STYLE), color },
    };
    if (ui.Focusable) return h(ui.Focusable, { ...common, noFocusRing: true, onActivate: open, onOKActionDescription: 'Vibeshine' }, streamIcon());
    return h('div', { ...common, tabIndex: 0, onClick: open }, streamIcon());
  }

  // Arrays of children reachable from an element (depth-limited), in tree order.
  function childArrays(node, out = [], depth = 0) {
    if (depth > 8 || node == null || typeof node !== 'object') return out;
    if (Array.isArray(node)) {
      out.push(node);
      for (const child of node) childArrays(child, out, depth + 1);
    } else if (React.isValidElement(node)) {
      childArrays(node.props?.children, out, depth + 1);
    }
    return out;
  }
  const isClockSlot = (el) => React.isValidElement(el) && [el.type, el.props?.children?.type].some((t) => (ui.clock && t === ui.clock) || /\.Clock\b/.test(srcOf(t)));
  const ICON_KEY = 'vibeshine-indicator';
  const isOurIcon = (el) => React.isValidElement(el) && el.key === ICON_KEY;

  // A header on screen keeps the element we put in it, even after this injection is replaced, and
  // Steam may keep the list holding it between renders. So that element is a slot shared by every
  // injection into this Steam UI: it shows the current injection's icon and redraws when that changes.
  // It does nothing else, since the first injection's copy is the one that stays.
  const SLOT_VERSION = 1;
  function headerSlot() {
    const existing = window.__vibeshineHeaderSlot;
    if (existing?.version === SLOT_VERSION) return existing;
    const shared = { version: SLOT_VERSION, content: null, listeners: new Set() };
    const subscribe = (fn) => {
      shared.listeners.add(fn);
      return () => shared.listeners.delete(fn);
    };
    const get = () => shared.content;
    shared.Slot = function VibeshineHeaderSlot() {
      const Content = React.useSyncExternalStore(subscribe, get);
      return Content ? h(Content) : null;
    };
    window.__vibeshineHeaderSlot = shared;
    return shared;
  }
  function showInSlot(shared, content) {
    shared.content = content;
    for (const fn of shared.listeners) {
      try {
        fn();
      } catch (e) {
        recordError('header-slot', e);
      }
    }
  }
  const HeaderIconContent = () => safe('header-icon', h(HeaderIcon));
  let slot = null; // Set once the header is patched.

  function insertHeaderIcon(bar) {
    const arrays = childArrays(bar);
    // Steam may return the same children array on every render (memoized, or hoisted by its
    // compiler), so the slot from an earlier render or injection can still be in it.
    for (const list of arrays) {
      const i = list.findIndex(isOurIcon);
      if (i < 0) continue;
      // An icon from a script with another slot would stay frozen: put ours in its place.
      if (list[i].type !== slot.Slot && !Object.isFrozen(list)) list[i] = h(slot.Slot, { key: ICON_KEY });
      return;
    }
    const lists = arrays.filter((list) => !Object.isFrozen(list));
    const icon = h(slot.Slot, { key: ICON_KEY });
    for (const list of lists) {
      const i = list.findIndex(isClockSlot);
      if (i >= 0) {
        list.splice(i, 0, icon);
        return setFeature('header_icon', 'ok (left of the clock)');
      }
    }
    const longest = lists.sort((a, b) => b.length - a.length)[0];
    if (longest && longest.length >= 3) {
      longest.push(icon);
      return setFeature('header_icon', 'ok (end of header; clock not recognised)');
    }
    setFeature('header_icon', 'off: header layout not recognised');
  }

  // ---------------------------------------------------------------------------------------------
  // Learning from Big Picture on screen. ES-module builds don't export the header and spell out class
  // names where they are used, so the header, its clock, Steam's header item component and the
  // context with the Steam window are read from the React tree Big Picture shows, climbing from the
  // clock: the element showing the time.
  // ---------------------------------------------------------------------------------------------
  const TIME_TEXT = /^\D{0,8}\d{1,2}[:.]\d{2}\D{0,8}$/;
  const PROVIDER = Symbol.for('react.provider');
  const isComponentType = (t) => typeof t === 'function' || t?.$$typeof === MEMO || t?.$$typeof === FORWARD_REF;
  function fiberOf(node) {
    const key = Object.keys(node).find((k) => k.startsWith('__reactFiber$'));
    return key ? node[key] : null;
  }
  function bigPictureDocuments() {
    const windowStore = window.SteamUIStore?.WindowStore;
    const windows = [windowStore?.GamepadUIMainWindowInstance, ...(windowStore?.SteamUIWindows ?? [])];
    return [...new Set(windows.map((w) => w?.BrowserWindow?.document).filter(Boolean))];
  }
  function learnFromScreen() {
    if (simulateMissing.has('header')) return false;
    for (const doc of bigPictureDocuments()) {
      for (const node of doc.querySelectorAll('*')) {
        if (node.childElementCount || !TIME_TEXT.test(node.textContent ?? '')) continue;
        const path = [];
        let fiber = fiberOf(node);
        while (fiber && !isHeaderBar(fiber.elementType) && path.length < 200) {
          path.push(fiber);
          fiber = fiber.return;
        }
        if (!isHeaderBar(fiber?.elementType)) continue;
        HeaderBar = fiber.elementType;
        // Steam wraps each header item in an error boundary: inside it the clock, and inside that the
        // component every header item draws with.
        const boundary = path.findIndex((f) => f.elementType?.prototype?.isReactComponent);
        if (boundary >= 2 && isComponentType(path[boundary - 1].elementType) && isComponentType(path[boundary - 2].elementType)) {
          ui.clock = path[boundary - 1].elementType;
          ui.headerItem = path[boundary - 2].elementType;
        }
        // Above the header: the Steam window it is drawn for.
        for (let f = fiber.return; f && !ui.windowContext; f = f.return) {
          const t = f.elementType;
          if ((t?.$$typeof === CONTEXT || t?.$$typeof === PROVIDER) && f.memoizedProps?.value?.BrowserWindow) ui.windowContext = t._context ?? t;
        }
        updateHeaderStylesFeature();
        updateHeaderMenuFeature();
        return true;
      }
    }
    return false;
  }

  function updateHeaderStylesFeature() {
    setFeature('header_styles', ui.headerCss ? 'ok' : ui.headerItem ? "ok (Steam's header item)" : 'fallback: inline styles');
  }

  function setupHeader() {
    if (!featureEnabled('header_icon')) {
      setFeature('header_menu', 'off: turned off in Vibeshine');
      return setFeature('header_icon', 'off: turned off in Vibeshine');
    }
    updateHeaderMenuFeature();
    if (!HeaderBar) learnFromScreen();
    if (!HeaderBar && esm && !options.checkOnly) {
      // Big Picture isn't showing its header yet. Look again until it does.
      const timer = setInterval(
        safeHandler('header-search', () => {
          if (!state.enabled || !learnFromScreen()) return;
          clearInterval(timer);
          patchHeader();
        }),
        2000,
      );
      cleanups.push(() => clearInterval(timer));
      return setFeature('header_icon', 'waiting for Big Picture to show its header');
    }
    if (!HeaderBar) return setFeature('header_icon', esm ? 'off: Big Picture is not showing its header; open it and check again' : 'off: header component not found');
    if (options.checkOnly) return setFeature('header_icon', 'ready');
    patchHeader();
  }

  function patchHeader() {
    slot = headerSlot();
    const original = HeaderBar.type;
    const patched = disguise(function VibeshineHeaderBar(...args) {
      const bar = original.apply(this, args);
      // A header on screen keeps the wrapper it was created with, so the icon comes from whichever
      // injection is current, and none once the controls are off. decorateHeader records its errors.
      try {
        window.VibeshineBPM?.decorateHeader?.(bar);
      } catch {}
      return bar;
    }, original);
    HeaderBar.type = patched;
    // A slot already on screen, from an earlier injection, shows our icon straight away.
    const onScreen = slot.listeners.size > 0;
    showInSlot(slot, HeaderIconContent);
    cleanups.push(() => {
      if (HeaderBar.type === patched) HeaderBar.type = original;
      if (slot.content === HeaderIconContent) showInSlot(slot, null);
    });
    // React reads a memo component's inner type only when the component is created, so a header that
    // is already on screen without our wrapper keeps rendering without the icon until Big Picture opens again.
    setFeature('header_icon', onScreen ? 'ok (already in the header)' : 'ready; appears the next time Big Picture opens');
  }

  // ---------------------------------------------------------------------------------------------
  // Start
  // ---------------------------------------------------------------------------------------------
  setFeature('focusable', ui.Focusable ? 'ok' : 'fallback: plain clickable icon (no controller focus)');
  updateHeaderStylesFeature();

  for (const [name, setup] of [['power_menu', setupPowerMenu], ['header_icon', setupHeader]]) {
    try {
      setup();
    } catch (e) {
      setFeature(name, `off: setup failed (${e.message})`);
      recordError(`setup ${name}`, e);
    }
  }

  const report = () => ({ protocol: PROTOCOL, version: VERSION, react: React.version, enabled: state.enabled, errors: state.errors, features: { ...features } });
  if (options.checkOnly) return report();

  const staleTimer = setInterval(safeHandler('stale-check', checkStale), 1000);
  cleanups.push(() => clearInterval(staleTimer));

  const api = {
    protocol: PROTOCOL,
    version: VERSION,
    setStatus,
    heartbeat,
    report,
    // Called by the header wrapper of this injection or an earlier one.
    decorateHeader(bar) {
      if (!slot || !state.enabled || !React.isValidElement(bar)) return;
      try {
        insertHeaderIcon(bar);
      } catch (e) {
        recordError('header', e);
      }
    },
    unpatch() {
      disable('replaced', { notify: false });
      if (window.VibeshineBPM === api) delete window.VibeshineBPM;
    },
  };
  window.VibeshineBPM = api;
  state.started = true;
  console.log(TAG, 'injected', report());
  return report();
})();
