// A fake Steam client for testing assets/steam/big_picture_controls.js without Steam.
//
// The fake components behave like Steam's, and report the source text (toString) of the real
// minified components from the Steam client build of Oct 22 2026, so the script's lookups are tested
// against realistic code. The script runs in its own VM context, like Steam's shared JS context.
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const SCRIPT = readFileSync(
  new URL('../../../../../windows/assets/steam/big_picture_controls.js', import.meta.url),
  'utf8',
);

// ---------------------------------------------------------------------------------------------
// Minimal React: enough element semantics for the script, plus a synchronous test renderer.
// ---------------------------------------------------------------------------------------------
const ELEMENT = Symbol.for('react.transitional.element');
const FRAGMENT = Symbol.for('react.fragment');
const MEMO = Symbol.for('react.memo');
const FORWARD_REF = Symbol.for('react.forward_ref');
const CONTEXT = Symbol.for('react.context');

class Component {
  props: any;
  state: any;
  constructor(props: any) {
    this.props = props;
  }
}
(Component.prototype as any).isReactComponent = {};

function withChildren(props: any, children: any[]) {
  if (children.length === 1) props.children = children[0];
  else if (children.length > 1) props.children = children;
  return props;
}

function createElement(type: any, config?: any, ...children: any[]) {
  const { key = null, ...props } = config ?? {};
  return {
    $$typeof: ELEMENT,
    type,
    key: key == null ? null : String(key),
    props: withChildren(props, children),
  };
}

function cloneElement(element: any, config?: any, ...children: any[]) {
  const { key, ...overrides } = config ?? {};
  return {
    $$typeof: ELEMENT,
    type: element.type,
    key: key ?? element.key,
    props: withChildren({ ...element.props, ...overrides }, children),
  };
}

const isValidElement = (value: any) => value?.$$typeof === ELEMENT;

export interface HostNode {
  tag: string;
  props: Record<string, any>;
  children: Array<HostNode | string>;
}

// ---------------------------------------------------------------------------------------------
// A retained renderer with the React behaviour the script depends on: a component redraws only
// when its store changes or its parent redraws it with a new element, an element identical to the
// previous one is skipped (so a list Steam keeps between renders is never looked at again), a memo
// component keeps the inner type it was created with, and error boundaries catch what throws below
// them. Updates run synchronously, and store subscriptions start once a render has finished.
// A context value is read when a component draws; a new value doesn't redraw what reads it.
// ---------------------------------------------------------------------------------------------
type Tag =
  | 'empty'
  | 'text'
  | 'host'
  | 'list'
  | 'fragment'
  | 'provider'
  | 'function'
  | 'memo'
  | 'forward'
  | 'class';

interface StoreHook {
  subscribe: (onChange: () => void) => () => void;
  getSnapshot: () => unknown;
  value: unknown;
  unsubscribe?: () => void;
}

interface Fiber {
  tag: Tag | 'root';
  node: any; // What this fiber was last rendered from.
  key: string | null;
  kind: unknown; // Element type, compared to decide whether a new element updates this fiber.
  render?: (props: any) => any; // For function components: memo's inner type, captured at mount.
  props?: any;
  instance?: any;
  stores: StoreHook[];
  children: Fiber[];
  parent: Fiber | null;
  alive: boolean;
}

let currentFiber: Fiber | null = null;
let hookIndex = 0;
let rendering = 0;
let created: Fiber[] = []; // Fibers made by the current render, dropped if a boundary catches.
const pendingSubscriptions: Array<[Fiber, StoreHook]> = [];
const dirty = new Set<Fiber>();

function describe(node: any): { tag: Tag; kind: unknown; key: string | null } {
  if (node == null || typeof node === 'boolean') return { tag: 'empty', kind: null, key: null };
  if (typeof node === 'string' || typeof node === 'number')
    return { tag: 'text', kind: null, key: null };
  if (Array.isArray(node)) return { tag: 'list', kind: null, key: null };
  if (!isValidElement(node)) throw new Error(`not renderable: ${String(node)}`);
  const { type, key } = node;
  if (type === FRAGMENT) return { tag: 'fragment', kind: FRAGMENT, key };
  if (type?.$$typeof === CONTEXT) return { tag: 'provider', kind: type, key };
  if (typeof type === 'string') return { tag: 'host', kind: type, key };
  if (type?.$$typeof === MEMO) return { tag: 'memo', kind: type, key };
  if (type?.$$typeof === FORWARD_REF) return { tag: 'forward', kind: type, key };
  if (typeof type === 'function' && type.prototype?.isReactComponent)
    return { tag: 'class', kind: type, key };
  if (typeof type === 'function') return { tag: 'function', kind: type, key };
  throw new Error('unknown element type');
}

const childList = (children: any): any[] =>
  children === undefined ? [] : Array.isArray(children) ? children : [children];

function shallowEqual(a: any, b: any): boolean {
  const keys = Object.keys(a ?? {});
  return (
    keys.length === Object.keys(b ?? {}).length && keys.every((key) => Object.is(a[key], b[key]))
  );
}

function kill(fiber: Fiber): void {
  if (!fiber.alive) return;
  fiber.alive = false;
  for (const hook of fiber.stores) hook.unsubscribe?.();
  fiber.children.forEach(kill);
}

function reconcile(parent: Fiber, old: Fiber[], nodes: any[]): Fiber[] {
  const slotOf = (key: string | null, index: number) => (key == null ? `#${index}` : `k:${key}`);
  const previous = new Map(old.map((fiber, index) => [slotOf(fiber.key, index), fiber]));
  const next = nodes.map((node, index) => {
    const slot = slotOf(describe(node).key, index);
    const match = previous.get(slot);
    previous.delete(slot);
    return build(parent, match, node);
  });
  previous.forEach(kill);
  return next;
}

/** Update `old` from `node` when it can, else replace it. `force` redraws even an unchanged element. */
function build(parent: Fiber, old: Fiber | undefined, node: any, force = false): Fiber {
  if (old?.alive && old.node === node && !force) return old;
  const { tag, kind, key } = describe(node);
  if (old?.alive && old.tag === tag && old.kind === kind && old.key === key) {
    update(old, node, force);
    return old;
  }
  if (old) kill(old);
  const fiber: Fiber = { tag, node, key, kind, stores: [], children: [], parent, alive: true };
  created.push(fiber);
  if (tag === 'memo') fiber.render = node.type.type;
  else if (tag === 'forward') fiber.render = (props) => node.type.render(props, null);
  else if (tag === 'function') fiber.render = node.type;
  else if (tag === 'class') fiber.instance = new node.type(node.props);
  update(fiber, node, true);
  return fiber;
}

function update(fiber: Fiber, node: any, force: boolean): void {
  const before = fiber.props;
  fiber.node = node;
  switch (fiber.tag) {
    case 'empty':
    case 'text':
      return;
    case 'host':
    case 'fragment':
    case 'provider':
      fiber.props = node.props;
      fiber.children = reconcile(fiber, fiber.children, childList(node.props.children));
      return;
    case 'list':
      fiber.children = reconcile(fiber, fiber.children, node);
      return;
    case 'memo':
      fiber.props = node.props;
      if (!force && shallowEqual(before, node.props)) return;
      renderFunction(fiber);
      return;
    case 'function':
    case 'forward':
      fiber.props = node.props;
      renderFunction(fiber);
      return;
    case 'class':
      fiber.props = node.props;
      fiber.instance.props = node.props;
      renderClass(fiber);
      return;
  }
}

function renderFunction(fiber: Fiber): void {
  const outer = [currentFiber, hookIndex] as const;
  currentFiber = fiber;
  hookIndex = 0;
  let rendered: any;
  try {
    rendered = fiber.render!(fiber.props);
  } finally {
    [currentFiber, hookIndex] = outer;
  }
  fiber.children = reconcile(fiber, fiber.children, [rendered]);
}

const isBoundary = (fiber: Fiber) =>
  fiber.tag === 'class' && typeof (fiber.kind as any).getDerivedStateFromError === 'function';

function renderClass(fiber: Fiber): void {
  const mark = created.length;
  try {
    fiber.children = reconcile(fiber, fiber.children, [fiber.instance.render()]);
  } catch (error) {
    if (!isBoundary(fiber)) throw error;
    for (const lost of created.splice(mark)) lost.alive = false;
    showFallback(fiber, error);
  }
}

function showFallback(boundary: Fiber, error: unknown): void {
  const type = boundary.kind as any;
  boundary.children.forEach(kill);
  boundary.instance.state = {
    ...boundary.instance.state,
    ...type.getDerivedStateFromError(error),
  };
  boundary.instance.componentDidCatch?.(error);
  boundary.children = reconcile(boundary, [], [boundary.instance.render()]);
}

/** Redraw one component whose store changed; what it throws goes to the nearest boundary above. */
function redraw(fiber: Fiber): void {
  const mark = created.length;
  try {
    if (fiber.tag === 'class') renderClass(fiber);
    else renderFunction(fiber);
  } catch (error) {
    for (const lost of created.splice(mark)) lost.alive = false;
    let boundary = fiber.parent;
    while (boundary && !isBoundary(boundary)) boundary = boundary.parent;
    if (!boundary) throw error;
    showFallback(boundary, error);
  }
}

function storeChanged(fiber: Fiber, hook: StoreHook): void {
  if (!fiber.alive || Object.is(hook.getSnapshot(), hook.value)) return;
  dirty.add(fiber);
  if (!rendering) flush();
}

function commit(): void {
  created = [];
  for (const [fiber, hook] of pendingSubscriptions.splice(0)) {
    if (!fiber.alive || hook.unsubscribe) continue;
    hook.unsubscribe = hook.subscribe(() => storeChanged(fiber, hook));
    // The store may have changed between the render and the subscription.
    storeChanged(fiber, hook);
  }
}

function run(work: () => void): void {
  rendering += 1;
  try {
    work();
  } finally {
    rendering -= 1;
  }
  if (!rendering) {
    commit();
    flush();
  }
}

let flushing = false;
function flush(): void {
  if (flushing) return;
  flushing = true;
  try {
    for (const fiber of dirty) {
      dirty.delete(fiber);
      if (fiber.alive) run(() => redraw(fiber));
    }
  } finally {
    flushing = false;
  }
}

function useSyncExternalStore(
  subscribe: StoreHook['subscribe'],
  getSnapshot: StoreHook['getSnapshot'],
): unknown {
  const fiber = currentFiber;
  if (!fiber) return getSnapshot();
  const index = hookIndex++;
  let hook = fiber.stores[index];
  if (!hook || hook.subscribe !== subscribe) {
    hook?.unsubscribe?.();
    hook = { subscribe, getSnapshot, value: undefined };
    fiber.stores[index] = hook;
    pendingSubscriptions.push([fiber, hook]);
  }
  hook.getSnapshot = getSnapshot;
  hook.value = getSnapshot();
  return hook.value;
}

// As in React 19, a context is its own provider.
function createContext(defaultValue: unknown) {
  const context: any = { $$typeof: CONTEXT, defaultValue };
  context.Provider = context;
  return context;
}

function useContext(context: any): unknown {
  for (let fiber = currentFiber; fiber; fiber = fiber.parent) {
    if (fiber.tag === 'provider' && fiber.kind === context) return fiber.props.value;
  }
  return context.defaultValue;
}

const output = (fiber: Fiber): Array<HostNode | string> => {
  if (fiber.tag === 'text') return [String(fiber.node)];
  if (fiber.tag === 'host')
    return [
      { tag: fiber.kind as string, props: fiber.props, children: fiber.children.flatMap(output) },
    ];
  return fiber.children.flatMap(output);
};

export interface Mounted {
  /** What is on screen now. */
  nodes(): Array<HostNode | string>;
  /** Redraw the top component, as when its own state changes, and return what is on screen. */
  update(): Array<HostNode | string>;
  unmount(): void;
}

/** Keep an element tree on screen, so later store changes redraw it the way React would. */
export function mount(node: any): Mounted {
  const root: Fiber = {
    tag: 'root',
    node: null,
    key: null,
    kind: null,
    stores: [],
    children: [],
    parent: null,
    alive: true,
  };
  run(() => {
    root.children = [build(root, undefined, node)];
  });
  const mounted: Mounted = {
    nodes: () => root.children.flatMap(output),
    update() {
      run(() => {
        root.children = [build(root, root.children[0], node, true)];
      });
      return mounted.nodes();
    },
    unmount() {
      root.children.forEach(kill);
      root.children = [];
    },
  };
  return mounted;
}

/** Render an element tree once and return its host nodes. */
export function render(node: any): Array<HostNode | string> {
  const mounted = mount(node);
  const nodes = mounted.nodes();
  mounted.unmount();
  return nodes;
}

export const React = {
  createElement,
  cloneElement,
  isValidElement,
  Fragment: FRAGMENT,
  Component,
  memo: (type: any) => ({ $$typeof: MEMO, type, compare: null }),
  forwardRef: (render: any) => ({ $$typeof: FORWARD_REF, render }),
  createContext,
  useContext,
  useSyncExternalStore,
  useId: () => ':r0:',
  version: '19.1.1',
};
const h = createElement;

export function textOf(nodes: Array<HostNode | string>): string {
  return nodes.map((node) => (typeof node === 'string' ? node : textOf(node.children))).join('');
}

/** Menu item labels in order; separators appear as '----'. */
export function menuLabels(nodes: Array<HostNode | string>): string[] {
  const labels: string[] = [];
  const walk = (list: Array<HostNode | string>) => {
    for (const node of list) {
      if (typeof node === 'string') continue;
      if (node.props.role === 'separator') labels.push('----');
      else if (String(node.props.className ?? '').includes('contextMenuItem'))
        labels.push(textOf(node.children));
      else walk(node.children);
    }
  };
  walk(nodes);
  return labels;
}

export function findHost(
  nodes: Array<HostNode | string>,
  predicate: (node: HostNode) => boolean,
): HostNode | undefined {
  for (const node of nodes) {
    if (typeof node === 'string') continue;
    if (predicate(node)) return node;
    const found = findHost(node.children, predicate);
    if (found) return found;
  }
  return undefined;
}

// ---------------------------------------------------------------------------------------------
// Steam components
// ---------------------------------------------------------------------------------------------
function withSource<T extends object>(target: T, source: string): T {
  Object.defineProperty(target, 'toString', { value: () => source, configurable: true });
  return target;
}

const noop = () => {};

export interface FakeSteamOptions {
  /** Source text of the clock component; drop ".Clock" to simulate a renamed clock. */
  clockSource?: string;
  /** Reuse one element for the header's right-hand items on every render, as React compilers do
   * for static subtrees, so its children array is shared between renders. */
  hoistHeaderItems?: boolean;
}

export function createSteam(options: FakeSteamOptions = {}) {
  const messages: any[] = [];
  const consoleErrors: unknown[][] = [];
  const createdMenus: Array<{ element: any; target: any; window: unknown }> = [];
  const calls = { openPowerMenu: 0 };

  // Menus -------------------------------------------------------------------------------------
  class MenuItem extends Component {
    render() {
      const { children, disabled, tone, onSelected } = this.props;
      return h(
        'div',
        {
          className: `contextMenuItem${disabled ? ' disabled' : ''}`,
          'data-tone': tone,
          onSelected: disabled ? undefined : onSelected,
        },
        children,
      );
    }
  }
  withSource(
    MenuItem,
    'class L extends l.PureComponent{static contextType=j;m_ref=l.createRef();m_refDiv=l.createRef();OnClick(re){this.props.onSelected?.(re)}render(){return(0,e.jsx)("div",{ref:this.m_refDiv,className:"contextMenuItem",children:this.props.children})}}',
  );

  const MenuSeparator = withSource(
    () => h('div', { className: 'ContextMenuSeparator', role: 'separator' }),
    'function O(re){const oe=l.useContext(j).styles??A();return(0,e.jsx)("div",{className:oe.ContextMenuSeparator,role:"separator"})}',
  );

  // Steam's Menu adds its own trailing separator and Cancel item.
  const Menu = withSource((props: any) => {
    React.useId();
    return h(
      'div',
      { className: 'contextMenu', 'aria-label': props.label },
      props.children,
      h(MenuSeparator, {}),
      h(MenuItem, { onSelected: props.onCancel ?? noop }, 'Cancel'),
    );
  }, 'function E(re){const Z=l.useId();return(0,e.jsx)(x,{labelId:Z,...re})}');

  // The Power menu is a MobX observer: a memo whose inner function doesn't contain the menu's text.
  const basePowerMenu = (props: any) =>
    h(
      Menu,
      { label: 'Power', onCancel: props.onCancel },
      [],
      h(MenuItem, { onSelected: noop }, 'Suspend System'),
      h(MenuItem, { onSelected: noop }, 'Turn Off System'),
      h(MenuItem, { onSelected: noop }, 'Restart System'),
      h(MenuSeparator, {}),
      h(
        FRAGMENT,
        null,
        h(MenuItem, { onSelected: noop }, 'Minimize Steam'),
        h(MenuItem, { onSelected: noop }, 'Exit Big Picture Mode'),
        h(MenuItem, { onSelected: noop }, 'Exit Steam'),
      ),
    );
  const PowerMenu = React.memo(
    withSource(
      (props: any) => basePowerMenu(props),
      'function(e,t){return useObserver(()=>baseComponent(e,t),baseComponentName)}',
    ),
  );

  class CContextMenuManager {
    CreateContextMenuInstance(element: any, target: any) {
      // Steam shows a menu in the window of the element it is opened on, and in the main Big
      // Picture window (Steam's shared context) for anything else.
      createdMenus.push({ element, target, window: target?.ownerDocument?.defaultView || window });
      return { Show: noop, Hide: noop };
    }
  }
  withSource(
    CContextMenuManager,
    'class i{m_rgMenuInstances=[];CreateContextMenuInstance(e,t,r,n,o){const s=new d(e,t,r,n,o,this);return this.m_rgMenuInstances.push(s),s}}',
  );
  const manager = new CContextMenuManager();
  const registry = { GetContextMenuManagerFromWindow: () => manager };
  const showContextMenu = withSource((element: any, target: unknown) => {
    const instance = registry
      .GetContextMenuManagerFromWindow()
      .CreateContextMenuInstance(element, target);
    instance.Show();
    return instance;
  }, 'function n(c,o,u){let b,d,S,g=o;if(g?.preventDefault&&g?.stopPropagation){if(g.shiftKey||g.altKey&&!u?.bRootContextMenu)return null;g.preventDefault(),g.stopPropagation(),S=g.currentTarget,b=g.clientX,d=g.clientY}else S=o;let y=i.GetContextMenuManagerFromWindow(S).CreateContextMenuInstance(c,S,b,d,u);return y.Show(),u?.bCreateHidden&&y.Hide(),y}');

  // Header ------------------------------------------------------------------------------------
  const headerCss = {
    HeaderItem: '_2HnVd7wV6yVaTiXfrBe3gq',
    Clickable: '_8sPcK-HDVUh0QbFshamLa',
    Clock: '_1HhLUvHH6BZLIOyOE80TVh',
    DashboardBar: '_3dBd9Rr2Dy5Lr9yXcF0aV',
  };

  const Focusable = withSource((props: any) => {
    const {
      'flow-children': _flowChildren,
      onCancel: _onCancel,
      focusClassName: _focusClassName,
      focusWithinClassName: _focusWithinClassName,
      noFocusRing: _noFocusRing,
      onOKActionDescription: _okDescription,
      children,
      ...rest
    } = props;
    return h('div', { ...rest, 'data-focusable': true }, children);
  }, 'function S(g){const{"flow-children":m,onActivate:y,onCancel:h,focusClassName:F,focusWithinClassName:T,...A}=g,{elemProps:M,navOptions:f,gamepadEvents:w}=(0,n.sl)(A);let C={};const I=(0,c.O)(m)}');

  // Steam wraps every header item in an error boundary that renders its children.
  class ItemBoundary extends Component {
    render() {
      return this.props.children;
    }
  }
  withSource(
    ItemBoundary,
    'class u extends a.Component{state={};constructor(h){super(h),this.state.lastErrorKey=h.errorKey}render(){return this.props.children}}',
  );
  const item = (name: string, source: string) =>
    withSource(
      () => h('div', { className: headerCss.HeaderItem, 'aria-label': name }, name),
      source,
    );
  const Search = item('Search', 'function oe(){return(0,e.jsx)(se,{className:Ne().SearchIcon})}');
  const Network = item(
    'Internet Settings',
    'function ui(qr){return(0,e.jsx)(se,{className:Ne().UpdatesIcon,description:(0,E.we)("#HeaderActions_Network")})}',
  );
  const Notifications = item(
    'Notifications',
    'function ai(qr){return(0,e.jsx)(se,{className:Ne().Bell})}',
  );
  const Account = item(
    'View Account',
    'function yi(qr){return(0,e.jsx)(se,{className:Ne().Avatar})}',
  );
  const Clock = withSource(
    () => h('div', { className: `${headerCss.HeaderItem} ${headerCss.Clock}` }, '4:06 PM'),
    options.clockSource ??
      'function gi(){const qr=(0,ae.hj)(),ni=(0,ae.I_)(),{region:Di}=a.useContext(mn),Pi=en(Di);return(0,e.jsx)(se,{className:(0,x.A)(Ne().Clock,Pi&&Ne().DashboardBar),vrTooltip:ni,children:qr})}',
  );
  const Provider = (props: any) => props.children;
  const HeaderRow = (props: any) => h('div', { id: 'header' }, props.children);
  const rightItems = () =>
    h(
      FRAGMENT,
      null,
      h(ItemBoundary, null, h(Notifications)),
      h(ItemBoundary, null, h(Clock)),
      h(ItemBoundary, null, h(Account)),
    );
  const hoistedRightItems = options.hoistHeaderItems ? rightItems() : undefined;
  const HeaderBar = React.memo(
    withSource(
      (_props: any) =>
        h(
          Provider,
          { value: { region: 0 } },
          h(
            HeaderRow,
            null,
            h(FRAGMENT, null, h(ItemBoundary, null, h(Search)), h(ItemBoundary, null, h(Network))),
            hoistedRightItems ?? rightItems(),
          ),
        ),
      'function(ni){let Di=ni.quickAccessHeader;(0,r.Qf)();const Pi=(0,$.oH)(),Gi=(0,y.Wx)(),cr=(0,$.we)();return(0,M.q3)(()=>Di?ni.visible:cr.BShowHeader())?(0,e.jsx)(mn.Provider,{value:{region:ni.quickAccessHeader?1:0},children:(0,e.jsxs)(X,{children:[]})}):null}',
    ),
  );

  // The Steam window everything below it is drawn for. In a game that is the game's overlay, also
  // for the Quick Access Menu drawn over it.
  const WindowContext = React.createContext(null);

  // Webpack registry --------------------------------------------------------------------------
  // Decoys first, so the lookups have to skip near misses and a module that throws when required.
  const modules: Array<[string, string, () => unknown]> = [
    ['1', 'function(e,t,n){t.onSelected=function(){}}', () => ({ x: noop })],
    ['2', 'e.exports={HeaderItem:"_x",Clock:"_y"}', () => ({ HeaderItem: '_x', Clock: '_y' })],
    [
      '3',
      'function(e,t,n){const CreateContextMenuInstance=1}',
      () => {
        throw new Error('module 3 fails to load');
      },
    ],
    [
      '62540',
      'function(e,t){var n=Symbol.for("react.transitional.element");t.createElement=function(e,t,r){};t.forwardRef=function(e){};t.useSyncExternalStore=function(e,t,n){return R.H.useSyncExternalStore(e,t,n)};t.version="19.1.1"}',
      () => React,
    ],
    [
      '12711',
      'function(e,t,n){n.d(t,{tz:()=>E,kt:()=>L,K5:()=>O});/* onSelected className:"contextMenuItem" */}',
      () => ({ tz: Menu, kt: MenuItem, K5: MenuSeparator }),
    ],
    [
      '38850',
      'function(e,t,n){n.d(t,{Z:()=>S});const{"flow-children":m,onActivate:y,onCancel:h,focusClassName:F,focusWithinClassName:T}=g}',
      () => ({ Z: Focusable }),
    ],
    [
      '30557',
      'e.exports={HeaderItem:"_2HnVd7wV6yVaTiXfrBe3gq",Clickable:"_8sPcK-HDVUh0QbFshamLa",Clock:"_1HhLUvHH6BZLIOyOE80TVh"}',
      () => headerCss,
    ],
    [
      '377',
      'function(e,t,n){n.d(t,{lX:()=>n,XX:()=>i});i.GetContextMenuManagerFromWindow(S).CreateContextMenuInstance(c,S,b,d,u)}',
      () => ({ lX: showContextMenu, XX: registry }),
    ],
    [
      '13240',
      'function(e,t,n){n.d(t,{z:()=>i});class i{CreateContextMenuInstance(e,t,r,n,o){}}}',
      () => ({ z: CContextMenuManager }),
    ],
    [
      '84978',
      'function(e,t,n){n.d(t,{hB:()=>xn});let Di=ni.quickAccessHeader;cr.BShowHeader()}',
      () => ({ hB: HeaderBar }),
    ],
    [
      '29827',
      '(xe,$,t)=>{"use strict";t.d($,{$2:()=>n,ER:()=>i,mA:()=>s});var e=t(62540),a=t(63696),r=t(91974);const s=a.createContext(null);function i(l){return(0,e.jsx)(s.Provider,{value:l.instance,children:(0,e.jsx)(r.F6,{factory:l.instance.VirtualKeyboardManager,children:l.children})})}const n=()=>a.useContext(s)}',
      () => ({ $2: () => React.useContext(WindowContext), ER: noop, mA: WindowContext }),
    ],
    [
      '79100',
      'function(e,t,n){n.d(t,{d4:()=>Te});(0,We.we)("#StopStreamingTo",Se.clientName);isPowerChord:kt}',
      () => ({ d4: noop }),
    ],
  ];
  const require: any = (id: string) => {
    const entry = modules.find(([moduleId]) => moduleId === id);
    if (!entry) throw new Error(`no module ${id}`);
    return entry[2]();
  };
  require.m = Object.fromEntries(
    modules.map(([id, source]) => [id, withSource(noop.bind(null), source)]),
  );

  // Steam's clock and timers, moved only by advance() and skip() ------------------------------
  let clock = 1_000_000;
  let nextTimer = 1;
  const timers = new Map<number, { fn: () => void; every: number; due: number }>();

  // Steam's shared JS context -----------------------------------------------------------------
  const window: any = vm.createContext({
    setInterval: (fn: () => void, every: number) => {
      timers.set(nextTimer, { fn, every, due: clock + every });
      return nextTimer++;
    },
    clearInterval: (id: number) => timers.delete(id),
    webpackChunksteamui: { push: (chunk: any[]) => chunk[2]?.(require) },
    __vibeshineBridge: (json: string) => messages.push(JSON.parse(json)),
    SteamUIStore: {
      OpenPowerMenu(target: unknown) {
        calls.openPowerMenu += 1;
        showContextMenu(h(PowerMenu, { onCancel: noop, isPowerChord: false }), target);
      },
    },
    console: { log: noop, error: (...args: unknown[]) => consoleErrors.push(args) },
  });
  window.window = window;
  vm.runInContext('Date', window).now = () => clock;

  return {
    window,
    timers,
    /** Move Steam's clock forward, running the timers that come due. */
    advance(ms: number) {
      const end = clock + ms;
      for (;;) {
        let next: { fn: () => void; every: number; due: number } | undefined;
        for (const timer of timers.values()) {
          if (timer.due <= end && (!next || timer.due < next.due)) next = timer;
        }
        if (!next) break;
        clock = next.due;
        next.due += next.every;
        next.fn();
      }
      clock = end;
    },
    /** Move Steam's clock forward without running timers, as when Chromium throttles them. */
    skip(ms: number) {
      clock += ms;
    },
    messages,
    consoleErrors,
    createdMenus,
    calls,
    components: { Menu, MenuItem, MenuSeparator, PowerMenu, HeaderBar, CContextMenuManager },
    originals: {
      createContextMenuInstance: CContextMenuManager.prototype.CreateContextMenuInstance,
      headerType: HeaderBar.type,
    },
    /** Evaluate the script inside Steam with the given options; returns its report as plain JSON. */
    inject(scriptOptions?: Record<string, unknown>) {
      if (scriptOptions) window.__vibeshineOptions = scriptOptions;
      return JSON.parse(JSON.stringify(vm.runInContext(SCRIPT, window)));
    },
    /** Open the Power menu the way Steam does and render the menu that was created. */
    openPowerMenu() {
      window.SteamUIStore.OpenPowerMenu();
      return render(createdMenus.at(-1)!.element);
    },
    /** Draw the header, for the given Steam window when there is one. */
    renderHeader(steamWindow?: unknown) {
      const header = h(HeaderBar, { quickAccessHeader: false });
      return render(steamWindow ? h(WindowContext, { value: steamWindow }, header) : header);
    },
    /** Keep the header on screen, as Big Picture does while it is open. */
    mountHeader() {
      return mount(h(HeaderBar, { quickAccessHeader: false }));
    },
  };
}

export type FakeSteam = ReturnType<typeof createSteam>;
