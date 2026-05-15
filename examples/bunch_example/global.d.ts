// Positron Bunch Module Types
// Auto-copied by `bunch create`. Provides editor autocompletion.

// ---------------------------------------------------------------------------
// File system helpers (api.fs)
// ---------------------------------------------------------------------------

interface PositronFs {
  read(path: string, encoding?: string): string | null;
  write(path: string, data: string | Buffer): void;
  exists(path: string): boolean;
  mkdir(path: string): void;
  readdir(path: string): string[];
  remove(path: string): void;
  stat(path: string): any;
  readFileSync: any;
  writeFileSync: any;
  existsSync: any;
  mkdirSync: any;
  readdirSync: any;
  rmSync: any;
  statSync: any;
  [key: string]: any;
}

// ---------------------------------------------------------------------------
// Module API (base)
// ---------------------------------------------------------------------------

interface PositronModuleApi {
  eval(code: string): Promise<any>;
  evalRenderer(code: string, windowIndex?: number): Promise<any>;
  send(msg: any): void;
  log(message: string): void;
  require: any;
  fs: PositronFs;
  path: any;
  os: any;
  childProcess: any;
  http: any;
  https: any;
  url: any;
  crypto: any;
  process: any;
  Buffer: any;
  electron: any;
  getElectron(): any;
  getWindows(): any[];
  getWebContents(index?: number): any;
  exec(command: string): string | null;
  setTimeout: typeof globalThis.setTimeout;
  setInterval: typeof globalThis.setInterval;
  clearTimeout: typeof globalThis.clearTimeout;
  clearInterval: typeof globalThis.clearInterval;
  [key: string]: any;
}

// ---------------------------------------------------------------------------
// VFS (Virtual File System)
// ---------------------------------------------------------------------------

interface PositronVFS {
  /** Read a file from VFS. Returns null if not found. */
  read(path: string): string | null;
  /** Write a file to VFS. */
  write(path: string, data: string): string;
  /** List files matching a prefix. */
  list(prefix?: string): string[];
  /** Total size of all VFS files (chars). */
  fullsize(): number;
  /** Size of a single VFS file (chars). */
  filesize(path: string): number;
  /** Execute a VFS JS file in main process. */
  exec(path: string): Promise<any>;
  /** Execute a VFS JS file in renderer. */
  execRenderer(path: string, windowIndex?: number): Promise<any>;
}

// ---------------------------------------------------------------------------
// DOM helpers (api.dom)
// ---------------------------------------------------------------------------

interface PositronDOM {
  // --- query ---
  /** Query first matching element. Returns text, html, tag, id, className, attrs. */
  query(selector: string, windowIndex?: number): Promise<{
    text: string; html: string; tag: string; id: string; className: string;
    attrs: Record<string, string>;
  } | null>;
  /** Query all matching elements. Returns array of {text, tag, id, className}. */
  queryAll(selector: string, windowIndex?: number): Promise<{
    text: string; tag: string; id: string; className: string;
  }[]>;

  // --- modify ---
  setText(selector: string, text: string, windowIndex?: number): Promise<boolean>;
  setHTML(selector: string, html: string, windowIndex?: number): Promise<boolean>;
  setAttr(selector: string, attr: string, value: string, windowIndex?: number): Promise<boolean>;
  removeAttr(selector: string, attr: string, windowIndex?: number): Promise<boolean>;
  /** Set inline style property. e.g. dom.setStyle('#box', 'background', 'red') */
  setStyle(selector: string, prop: string, value: string, windowIndex?: number): Promise<boolean>;
  addClass(selector: string, cls: string, windowIndex?: number): Promise<boolean>;
  removeClass(selector: string, cls: string, windowIndex?: number): Promise<boolean>;
  toggleClass(selector: string, cls: string, windowIndex?: number): Promise<boolean>;
  hide(selector: string, windowIndex?: number): Promise<boolean>;
  show(selector: string, windowIndex?: number): Promise<boolean>;
  /** Remove element from DOM entirely. */
  remove(selector: string, windowIndex?: number): Promise<boolean>;
  /** Set input/textarea value and dispatch 'input' event. */
  setVal(selector: string, value: string, windowIndex?: number): Promise<boolean>;

  // --- inject HTML ---
  /** Create and append a new element to a parent. Returns outerHTML of created element. */
  append(parentSelector: string, tag: string, innerHTML?: string, attrs?: Record<string, string>, windowIndex?: number): Promise<string | null>;
  /** Create and prepend a new element to a parent. */
  prepend(parentSelector: string, tag: string, innerHTML?: string, attrs?: Record<string, string>, windowIndex?: number): Promise<string | null>;
  /** Insert raw HTML relative to an element. Position: 'beforebegin'|'afterbegin'|'beforeend'|'afterend'. */
  insertHTML(selector: string, position: 'beforebegin' | 'afterbegin' | 'beforeend' | 'afterend', html: string, windowIndex?: number): Promise<boolean>;
  /** Insert HTML from a VFS file. */
  injectHTMLFile(selector: string, position: 'beforebegin' | 'afterbegin' | 'beforeend' | 'afterend', vfsPath: string, windowIndex?: number): Promise<boolean>;
  /** Replace an element's outerHTML entirely. */
  replaceWith(selector: string, html: string, windowIndex?: number): Promise<boolean>;

  // --- inject CSS/JS ---
  /** Inject a raw CSS string as a <style> tag. Returns the style element id. */
  injectCSS(css: string, windowIndex?: number): Promise<string>;
  /** Inject CSS from a VFS file. */
  injectCSSFile(vfsPath: string, windowIndex?: number): Promise<string>;
  /** Remove a previously injected <style> by id. */
  removeCSS(id: string, windowIndex?: number): Promise<boolean>;
  /** Inject a raw JS string as a <script> tag. Returns the script element id. */
  injectScript(code: string, windowIndex?: number): Promise<string>;
  /** Inject JS from a VFS file as a <script> tag. */
  injectScriptFile(vfsPath: string, windowIndex?: number): Promise<string>;

  // --- events ---
  onClick(selector: string, handlerCode: string, windowIndex?: number): Promise<boolean>;
  /** Attach any event listener. handlerCode receives `ev` as the event object. */
  on(selector: string, event: string, handlerCode: string, windowIndex?: number): Promise<boolean>;

  // --- wait / observe ---
  /** Wait for element to appear. Resolves true if found, false on timeout. */
  waitFor(selector: string, timeoutMs?: number, windowIndex?: number): Promise<boolean>;

  // --- read page info ---
  getTitle(windowIndex?: number): Promise<string>;
  getURL(windowIndex?: number): Promise<string>;
  /** Get full page HTML (document.documentElement.outerHTML). */
  getHTML(windowIndex?: number): Promise<string>;
  /** Get element bounding rect. */
  getBounds(selector?: string, windowIndex?: number): Promise<{x:number;y:number;w:number;h:number;top:number;left:number} | null>;

  /**
   * Capture a screenshot of the renderer page via webContents.capturePage().
   *
   * @param opts.saveTo   — file path to save PNG/JPEG. If omitted, returns base64.
   * @param opts.format   — 'png' (default) or 'jpeg'
   * @param opts.quality  — JPEG quality 0-100 (default 80)
   * @param opts.rect     — capture a sub-region {x, y, width, height}
   * @param opts.windowIndex — BrowserWindow index (default 0)
   *
   * @returns {size, width, height, format, savedTo?, base64?}
   */
  screenshot(opts?: {
    saveTo?: string;
    format?: 'png' | 'jpeg';
    quality?: number;
    rect?: { x: number; y: number; width: number; height: number };
    windowIndex?: number;
  }): Promise<{
    size: number;
    width: number;
    height: number;
    format: string;
    savedTo?: string;
    base64?: string;
  }>;
}

// ---------------------------------------------------------------------------
// Auto-cleanup timer (api.timer)
// ---------------------------------------------------------------------------

interface PositronTimer {
  /** setInterval with auto-cleanup on module unload. */
  interval(fn: () => void, ms: number): number;
  /** setTimeout with auto-cleanup on module unload. */
  timeout(fn: () => void, ms: number): number;
  /** Manually clear a registered timer. */
  clear(id: number): void;
}

// ---------------------------------------------------------------------------
// Storage access (api.store)
// ---------------------------------------------------------------------------

interface PositronStorageBucket {
  get(key: string, windowIndex?: number): Promise<string | null>;
  set(key: string, value: string, windowIndex?: number): Promise<void>;
  keys(windowIndex?: number): Promise<string[]>;
  remove(key: string, windowIndex?: number): Promise<void>;
}

interface PositronStore {
  local: PositronStorageBucket;
  session: PositronStorageBucket;
}

// ---------------------------------------------------------------------------
// Bunch API (extends base with dom, timer, store, vfs)
// ---------------------------------------------------------------------------

interface PositronBunchApi extends PositronModuleApi {
  vfs: PositronVFS;
  dom: PositronDOM;
  timer: PositronTimer;
  store: PositronStore;
}

// ---------------------------------------------------------------------------
// Bunch Module definition
// ---------------------------------------------------------------------------

interface PositronBunchModule {
  /** Unique module name. */
  name: string;
  /** Called on load. VFS available via api.vfs. */
  onLoad?(api: PositronBunchApi): void | Promise<void>;
  /** Called on unload. Timers auto-cleared before this. */
  onUnload?(api: PositronBunchApi): void | Promise<void>;
  [key: string]: any;
}
