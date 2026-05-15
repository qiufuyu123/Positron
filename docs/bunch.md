# Positron Bunch — Module Bundler

Bunch is a build tool that packages multi-file TypeScript/JS projects into a single `.b.js` bundle that can be loaded by Positron's module system. It provides a virtual file system (VFS), DOM helpers, auto-cleanup timers, and more.

## Quick Start

```bash
# Create a new project
node tools/bunch/bunch.js create my_project

# Edit my_project/main.ts ...

# Build
node tools/bunch/bunch.js build my_project

# Load in Positron REPL
positron[pid]> .mod load my_project/.build/my_project.b.js
```

## Project Structure

```
my_project/
  bunch.json          # name, version, entry point
  global.d.ts         # type definitions (auto-generated)
  tsconfig.json       # VS Code intellisense
  main.ts             # entry point (exports BunchModule)
  data/               # static resources (HTML, CSS, etc.)
  *.ts                # additional TypeScript files
```

### bunch.json

```json
{
    "name": "my_project",
    "version": "0.0.1",
    "entry": "main.js"
}
```

## Entry Point

The entry file must declare a `BunchModule` variable:

```ts
const BunchModule: PositronBunchModule = {
  name: 'my_project',

  onLoad: function(api: PositronBunchApi) {
    // Access bundled files via api.vfs
    var html = api.vfs.read('data/overlay.html');

    // Execute bundled JS in renderer
    api.vfs.execRenderer('renderer_script', 0);

    // DOM manipulation
    api.dom.setText('#title', 'Injected!');

    // Auto-cleanup timer
    api.timer.interval(function() { /* ... */ }, 1000);
  },

  onUnload: function(api: PositronBunchApi) {
    api.dom.remove('#my_overlay');
  }
};
```

## Build Output

```
.build/
  obj/                # intermediate: tsc compiled .js files
  my_project.b.js     # final minified bundle
```

The bundle is a self-contained IIFE that:
1. Creates a VFS with all project files (base64 encoded)
2. Sets up the runtime (DOM helpers, timers, storage, screenshot)
3. Evaluates the entry module
4. Returns a standard `PositronModule` compatible with `.mod load`

## Bunch API (`PositronBunchApi`)

Extends the base `PositronModuleApi` with:

### VFS (`api.vfs`)

| Method | Description |
|---|---|
| `vfs.read(path)` | Read a bundled file as string |
| `vfs.write(path, data)` | Write to VFS (runtime only) |
| `vfs.list(prefix?)` | List bundled files |
| `vfs.exec(path)` | Execute a VFS JS file in main process |
| `vfs.execRenderer(path, idx?)` | Execute a VFS JS file in renderer |

### DOM (`api.dom`)

| Method | Description |
|---|---|
| `dom.setText(sel, text)` | Set element text |
| `dom.setHTML(sel, html)` | Set element innerHTML |
| `dom.query(sel)` / `dom.queryAll(sel)` | Query elements |
| `dom.append(parent, tag, html, attrs)` | Create and append element |
| `dom.insertHTML(sel, pos, html)` | insertAdjacentHTML |
| `dom.injectCSS(css)` / `dom.injectCSSFile(vfsPath)` | Inject styles |
| `dom.injectScript(code)` / `dom.injectScriptFile(vfsPath)` | Inject scripts |
| `dom.hide(sel)` / `dom.show(sel)` / `dom.remove(sel)` | Visibility |
| `dom.setStyle(sel, prop, val)` | Inline style |
| `dom.waitFor(sel, timeout?)` | Wait for element to appear |
| `dom.onClick(sel, code)` / `dom.on(sel, event, code)` | Events |
| `dom.screenshot(opts?)` | Capture page via `webContents.capturePage()` |
| `dom.getTitle()` / `dom.getURL()` / `dom.getHTML()` | Page info |

### Timer (`api.timer`)

| Method | Description |
|---|---|
| `timer.interval(fn, ms)` | setInterval with auto-cleanup on unload |
| `timer.timeout(fn, ms)` | setTimeout with auto-cleanup on unload |
| `timer.clear(id)` | Manual clear |

### Store (`api.store`)

| Method | Description |
|---|---|
| `store.local.get(key)` | Read renderer localStorage |
| `store.local.set(key, val)` | Write renderer localStorage |
| `store.local.keys()` | List localStorage keys |
| `store.session.*` | Same for sessionStorage |

### Base API (inherited)

All methods from `PositronModuleApi` are available: `api.eval()`, `api.evalRenderer()`, `api.send()`, `api.log()`, `api.fs`, `api.path`, `api.os`, `api.process`, `api.http`, `api.https`, `api.crypto`, `api.childProcess`, `api.exec()`, `api.electron`, `api.getElectron()`, `api.require`, `api.Buffer`.

## CLI Reference

```bash
# Create new project with boilerplate
node tools/bunch/bunch.js create <name>

# Build project (compile TS, bundle VFS, minify)
node tools/bunch/bunch.js build [dir]
```

## How It Works

1. **Collect**: Scans project directory for `.ts` and resource files (ignores `.build/`, `node_modules/`, `.d.ts`, `bunch.json`, `tsconfig.json`)
2. **Compile**: Runs `tsc` on all `.ts` files -> `.build/obj/*.js`
3. **Pack**: Base64-encodes all files into VFS write statements
4. **Wrap**: Embeds VFS entries into `runtime.js` template (provides dom/timer/store/vfs helpers)
5. **Entry**: Runtime evals the entry file, extracts `BunchModule`, wraps lifecycle
6. **Minify**: Runs `terser` with `--compress negate_iife=false,side_effects=false --mangle`
7. **Output**: `.build/<name>.b.js` -- loadable via `.mod load`
