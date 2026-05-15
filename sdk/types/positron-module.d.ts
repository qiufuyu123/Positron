/**
 * Positron Module API type definitions.
 *
 * These describe the `api` object passed to module `onLoad(api)` and
 * `onUnload(api)` lifecycle hooks. Use for editor autocompletion when
 * writing positron modules.
 *
 * Usage:
 *   /// <reference path="path/to/positron-module.d.ts" />
 *   // or add to tsconfig.json / jsconfig.json "include"
 */

// ---------------------------------------------------------------------------
// File system helpers (api.fs)
// ---------------------------------------------------------------------------

interface PositronFs {
  /** Read file synchronously. Defaults to utf8. */
  read(path: string, encoding?: string): string | null;
  /** Write data to file synchronously. */
  write(path: string, data: string | Buffer): void;
  /** Check if a path exists. */
  exists(path: string): boolean;
  /** Create directory recursively. */
  mkdir(path: string): void;
  /** List directory entries. */
  readdir(path: string): string[];
  /** Remove file or directory recursively. */
  remove(path: string): void;
  /** Get file/directory stats. */
  stat(path: string): import('fs').Stats | null;

  // Full Node.js fs module is also available:
  readFileSync: typeof import('fs').readFileSync;
  writeFileSync: typeof import('fs').writeFileSync;
  existsSync: typeof import('fs').existsSync;
  mkdirSync: typeof import('fs').mkdirSync;
  readdirSync: typeof import('fs').readdirSync;
  rmSync: typeof import('fs').rmSync;
  statSync: typeof import('fs').statSync;
  // ... all other fs methods
  [key: string]: any;
}

// ---------------------------------------------------------------------------
// Module API
// ---------------------------------------------------------------------------

interface PositronModuleApi {
  // --- Eval ---

  /** Evaluate JS code in the main process global scope. */
  eval(code: string): Promise<any>;

  /**
   * Evaluate JS code in a renderer process via
   * `webContents.executeJavaScript`.
   * @param code JS expression to evaluate
   * @param windowIndex BrowserWindow index (default 0)
   */
  evalRenderer(code: string, windowIndex?: number): Promise<any>;

  // --- Communication ---

  /** Send a `mod.event` frame to the host REPL. */
  send(msg: any): void;

  /** Send a log message to the host REPL. */
  log(message: string): void;

  // --- Node.js core modules ---

  /** The resolved `require` function (works in main process). */
  require: NodeRequire;

  /** File system module with convenience helpers. */
  fs: PositronFs;

  /** Node.js `path` module. */
  path: typeof import('path');

  /** Node.js `os` module. */
  os: typeof import('os');

  /** Node.js `child_process` module. */
  childProcess: typeof import('child_process');

  /** Node.js `http` module. */
  http: typeof import('http');

  /** Node.js `https` module. */
  https: typeof import('https');

  /** Node.js `url` module. */
  url: typeof import('url');

  /** Node.js `crypto` module. */
  crypto: typeof import('crypto');

  // --- Node.js globals ---

  /** Node.js `process` global. */
  process: NodeJS.Process | null;

  /** Node.js `Buffer` constructor. */
  Buffer: typeof Buffer | null;

  // --- Electron ---

  /** The Electron module (main process), or null. */
  electron: typeof import('electron') | null;

  /** Get the Electron module (lazy resolution). */
  getElectron(): typeof import('electron') | null;

  /** Get all BrowserWindow instances. */
  getWindows(): import('electron').BrowserWindow[];

  /**
   * Get a specific window's webContents.
   * @param index BrowserWindow index (default 0)
   */
  getWebContents(index?: number): import('electron').WebContents | null;

  // --- Convenience ---

  /**
   * Execute a shell command synchronously, returns stdout as string.
   * Wraps `child_process.execSync`.
   */
  exec(command: string): string | null;

  setTimeout: typeof globalThis.setTimeout;
  setInterval: typeof globalThis.setInterval;
  clearTimeout: typeof globalThis.clearTimeout;
  clearInterval: typeof globalThis.clearInterval;
}

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------

interface PositronModule {
  /** Unique module name. Used for `.mod unload <name>`. */
  name: string;

  /**
   * Called when the module is loaded. Use `api` to interact with the
   * target process. May return a Promise for async initialization.
   */
  onLoad?(api: PositronModuleApi): void | Promise<void>;

  /**
   * Called when the module is unloaded. Use `api` to clean up renderer
   * state, timers, etc. May return a Promise for async cleanup.
   */
  onUnload?(api: PositronModuleApi): void | Promise<void>;
  [key: string]: any;
}
// ---------------------------------------------------------------------------
// Bunch: Virtual File System
// ---------------------------------------------------------------------------

interface PositronVFS {
  /** Read a file from the virtual filesystem. Returns null if not found. */
  read(path: string): string | null;
  /** Write a file to the virtual filesystem. */
  write(path: string, data: string): string;
  /** List files matching a prefix. */
  list(prefix?: string): string[];
  /** Total size of all VFS files in characters. */
  fullsize(): number;
  /** Size of a single VFS file in characters. */
  filesize(path: string): number;
  /** Execute a VFS JS file in the main process. */
  exec(path: string): Promise<any>;
  /** Execute a VFS JS file in a renderer process. */
  execRenderer(path: string, windowIndex?: number): Promise<any>;
}

// ---------------------------------------------------------------------------
// Bunch: DOM helpers (injected by runtime onto api.dom)
// ---------------------------------------------------------------------------

interface PositronDOM {
  /** Query an element, returns text/html/attrs or null. */
  query(selector: string, windowIndex?: number): Promise<{
    text: string; html: string; attrs: Record<string, string>;
  } | null>;
  /** Set element textContent. */
  setText(selector: string, text: string, windowIndex?: number): Promise<boolean>;
  /** Set element innerHTML. */
  setHTML(selector: string, html: string, windowIndex?: number): Promise<boolean>;
  /** Set an attribute on an element. */
  setAttr(selector: string, attr: string, value: string, windowIndex?: number): Promise<boolean>;
  /** Add a CSS class. */
  addClass(selector: string, cls: string, windowIndex?: number): Promise<boolean>;
  /** Remove a CSS class. */
  removeClass(selector: string, cls: string, windowIndex?: number): Promise<boolean>;
  /** Inject a raw CSS string. Returns the style element id. */
  injectCSS(css: string, windowIndex?: number): Promise<string>;
  /** Inject CSS from a VFS file path. */
  injectCSSFile(vfsPath: string, windowIndex?: number): Promise<string>;
  /** Wait for an element to appear in the DOM. */
  waitFor(selector: string, timeoutMs?: number, windowIndex?: number): Promise<boolean>;
  /** Attach a click handler (pass JS code as string). */
  onClick(selector: string, handlerCode: string, windowIndex?: number): Promise<boolean>;
}

// ---------------------------------------------------------------------------
// Bunch: Auto-cleanup timer
// ---------------------------------------------------------------------------

interface PositronTimer {
  /** Like setInterval, but auto-cleared on module unload. */
  interval(fn: () => void, ms: number): number;
  /** Like setTimeout, but auto-cleared on module unload. */
  timeout(fn: () => void, ms: number): number;
  /** Manually clear a timer registered via interval/timeout. */
  clear(id: number): void;
}

// ---------------------------------------------------------------------------
// Bunch: Storage access (renderer localStorage / sessionStorage)
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
// Bunch: Extended API (PositronModuleApi + bunch extras)
// ---------------------------------------------------------------------------

interface PositronBunchApi extends PositronModuleApi {
  /** Virtual file system (bundled files). */
  vfs: PositronVFS;
  /** DOM manipulation helpers (renderer-side). */
  dom: PositronDOM;
  /** Auto-cleanup timers. */
  timer: PositronTimer;
  /** Renderer storage access. */
  store: PositronStore;
}

// ---------------------------------------------------------------------------
// Bunch: Module definition
// ---------------------------------------------------------------------------

interface PositronBunchModule {
  /** Unique module name. Used for `.mod unload <name>`. */
  name: string;

  /**
   * Called when the module is loaded. VFS available via api.vfs.
   */
  onLoad?(api: PositronBunchApi): void | Promise<void>;

  /**
   * Called when the module is unloaded. Timers are auto-cleared before
   * this is called.
   */
  onUnload?(api: PositronBunchApi): void | Promise<void>;
  [key: string]: any;
}
