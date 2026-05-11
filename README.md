# Positron

> Runtime JS injection toolkit for Electron apps on Windows

[中文文档](docs/README_CN.md)

Attach to any running Electron application, evaluate JavaScript in its main or renderer process, and extend functionality through a live module system — without restarting the target or modifying its files.

![Positron Demo](imgs/img1.png)
*Process listing and the demo fixture app*

## Features

- **Attach to running processes** — no restart, no source modification
- **Eval in main or renderer** — switch worlds with `.world renderer`
- **Module system** — load/unload JS modules with lifecycle hooks
- **Self-unmapping DLL** — payload erases itself from memory after bootstrap
- **Reconnect without re-inject** — JS server survives host disconnection
- **x64 and x86** support
- **C++ SDK** — static library for programmatic integration

## Quick Start

```bash
# Build (requires VS2022+ C++20, Node.js)
npm install
msbuild positron.slnx /p:Configuration=Debug /p:Platform=x64 /m

# List Electron processes
host.exe list

# Attach and start REPL
host.exe attach <pid>
```

## How It Works

```
host.exe ──attach──> target (Electron)
   │                    │
   │  1. Manual-map payload.dll (BlackBone, bypasses loader checks)
   │  2. Resolve V8 symbols via fuzzy PE export matching
   │  3. Bootstrap a JS TCP server in V8 main thread
   │  4. payload.dll self-unmaps (VEH removal + VirtualFree trampoline)
   │                    │
   │  5. Pure JS communication (length-prefixed JSON over TCP)
   │                    │
   └────────────────────┘
```

After bootstrap, the native DLL is completely gone from memory. Subsequent `host.exe attach` detects the live JS server and skips injection.

## Usage

### Interactive REPL

![REPL Demo](imgs/img2.png)
*Attaching, entering renderer world, reading document.title*

```
host.exe attach <pid>

positron[pid]> 1+1
2
positron[pid]> process.versions.electron
"42.0.1"
positron[pid]> .world renderer
[world = renderer:0]
positron[pid/r0]> document.title
"Positron Demo"
```

### Live DOM Manipulation

![DOM Injection](imgs/img3.png)
*Modifying the page title from the REPL*

```
positron[pid/r0]> document.title = "injected"
"injected"
```

### Module System

![Module Demo](imgs/img4.png)
*Demo module loaded: title changed, status badge flipped, log entries streaming*

```
positron[pid]> .mod load examples\modules\demo.js
[mod.load] {"loaded":"demo"}
positron[pid]> .mod list
[modules] ["demo"]
positron[pid]> .mod unload demo
[mod.unload] {"unloaded":"demo"}
```

Modules have full lifecycle management:

```js
(function() {
  return {
    name: 'my-module',
    onLoad: function(api) {
      api.evalRenderer('document.title', 0).then(function(t) {
        api.log('title: ' + t);
      });
    },
    onUnload: function(api) {
      api.log('bye');
    }
  };
})()
```

**Module API:** `api.eval()`, `api.evalRenderer()`, `api.send()`, `api.log()`, `api.getElectron()`, `api.require`

### One-shot Commands

```bash
host.exe eval <pid> "1+1"                  # main process
host.exe eval -r <pid> "document.title"    # renderer
host.exe run <pid> script.js               # run a JS file
```

### Native Mode

For hooking native exports (requires DLL to stay mapped):

```bash
host.exe attach --native <pid>
positron(native)[pid]> .hook SomeExport
```

## REPL Commands

| Command | Description |
|---|---|
| `.world auto\|renderer[:N]` | Switch eval target |
| `.mod load <file>` | Load JS module |
| `.mod unload <name>` | Unload module |
| `.mod list` | List loaded modules |
| `.dump [name]` | Save page HTML |
| `.hook <symbol>` | (native) Install detour |
| `.detach` | Disconnect |

## Architecture

```
positron/
  host/           CLI + REPL (host.exe)
  payload/        Injected DLL — V8 bridge, teardown trampoline
  sdk/            Static lib, headers, bootstrap.js (terser-minified)
  shared/wire/    Length-prefixed JSON framing
  examples/       SDK consumer, JS modules (demo, hello)
  tests/          Integration + unit tests
  third_party/    BlackBone, nlohmann/json, replxx, CLI11, MinHook
```

### Self-Unmap Sequence

After JS server confirms alive:

1. Stop detached threads (renderer poller, hook flusher)
2. `RemoveVectoredExceptionHandler` (BlackBone's VEH)
3. `VirtualFree` BlackBone scratch pages (~12 KB)
4. Shellcode trampoline on separate RWX page: `Sleep(500ms)` → `VirtualFree(payload_base)` → `ExitThread(0)`

## SDK

```cpp
#include <positron/sdk.h>

positron::sdk::Session s;
s.attach(pid);               // v2 by default
auto r = s.eval("1+1");     // r.json_value == "2"
s.detach();
```

## License

[MIT](LICENSE)
