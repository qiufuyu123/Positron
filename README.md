# positron

Windows-only injection framework for Electron programs. Attaches to a running
target, captures the V8 isolate, and lets you run arbitrary JS or hook native
exports — without depending on the target loading a NAPI native addon.

## What works

- **Attach to a running Electron process.** No target restart needed.
- **Run JS in the V8 main world via direct `v8::*` calls** — `Isolate::TryGetCurrent`,
  `Script::Compile`, `Script::Run` are resolved from the host binary's
  exported symbols at runtime via fuzzy substring matching, so the same
  payload works across V8 versions whose mangled names differ.
- **Inject into Chromium-process trees via Blackbone manual mapping** —
  bypasses the standard loader, so unsigned payload DLLs can be planted into
  modern Electron processes that block `LoadLibrary` on non-Microsoft binaries.
- **TCP-socket transport on localhost.** Port is derived from the target PID
  (no out-of-band parameter passing). Works through Chromium's renderer
  network restrictions when sandbox is off.
- **Inline runtime hooks** on any V8 / libuv export. Hits stream back as
  `hook.hit` events with the first four register-passed arguments.
- **Interactive REPL** via replxx, plus `eval` and `run` subcommands for one-shot
  scripts.

## Limitations

- **x64 only** in v1.
- **Sandboxed renderers** (`webPreferences.sandbox: true`, the Electron default)
  block named-pipe / socket / kernel-event creation from inside the renderer.
  Attach into renderers requires `sandbox: false`. Sandbox-on support is
  future work — needs pre-duplicated handles + a Mojo-aware injector.
- **Renderer main world targeting** under `contextIsolation: true`: payload
  currently runs in whatever V8 context happens to be entered when the async
  fires (often the isolated context). Bridging into the page's main world
  via `webFrame.executeJavaScript` is a future refinement.

## Build

Open `positron.slnx` in Visual Studio (v145 toolset, C++20). Target: x64.

Or from a Developer Command Prompt:

```
msbuild positron.slnx /p:Configuration=Debug /p:Platform=x64 /m
```

Outputs land in `x64\Debug\`:

- `host.exe` — the CLI / REPL
- `payload.dll` — the injected library

## Use

```
positron list                              # list candidate Electron processes
positron attach <pid>                      # inject + interactive REPL
positron eval   <pid> "<js-expression>"    # one-shot eval, print result, exit
positron run    <pid> path\to\script.js    # run a JS file, print result, exit
```

The `--dll <path>` flag overrides the default payload location (next to `host.exe`).

In the REPL:

```
positron[12345]> 1 + 1
2
positron[12345]> process.versions.electron
"42.0.1"
positron[12345]> .hook napi_throw
[hook.install queued: napi_throw]
positron[12345]> .detach
```

## Architecture

```
host.exe (CLI / REPL)                       target process (electron.exe)
  ┌─────────────────────────┐                ┌───────────────────────────┐
  │ CLI11 subcommands       │                │ payload.dll (mapped via   │
  │ replxx REPL             │                │   Blackbone, no loader)   │
  │ Blackbone injector ─────┼─────map──────► │   ├─ pe_resolver         │
  │ TCP listener (per-pid   │                │   ├─ v8_bridge           │
  │   port = 30000+pid%30k) │ ◄── 127.0.0.1 ─┤   │    (uv_async + V8 FFI)│
  │ replxx prompt           │   socket       │   ├─ hook_engine         │
  └─────────────────────────┘                │   │    (MinHook + ring)   │
                                             │   ├─ ipc_server          │
                                             │   │    (writer thread)   │
                                             │   └─ bootstrap            │
                                             └───────────────────────────┘
```

- **pe_resolver** parses the host binary's export table and exposes a
  fuzzy-substring resolver. v8_bridge uses it to find `Isolate::TryGetCurrent`
  / `Script::Compile` / `uv_default_loop` etc. across V8 versions without
  hard-coded mangled names.
- **v8_bridge** arms a `uv_async_t` on the default libuv loop. From any
  thread, schedule a callback onto the V8 thread; from there, run JS via
  the resolved exports. ABI-aware: V8's clang-cl-built member functions
  return `Local<T>` via Itanium-style `this`-first / hidden-ret-ptr layout
  rather than MSVC's standard, which v8_bridge's typedefs encode.
- **ipc_server** is a TCP **client** despite the name (historic — keeps
  consumer code unchanged): connects out to the host, sends HELLO with a
  list of resolved napi-style exports, then accepts commands. Outbound
  writes go through a dedicated writer thread + outbox, so the V8 thread
  never blocks on `send()`.
- **hook_engine** wraps MinHook for internal use, plus an 8-slot user pool.
  Detour template captures register args into a SPSC ring buffer; a
  flusher thread converts to `hook.hit` IPC events.

## Tests

```
x64\Debug\unit_host.exe       # wire protocol, PE resolver, process_enum
x64\Debug\unit_payload.exe    # ring buffer (incl. SPSC concurrency)
x64\Debug\integration.exe     # spawns Electron fixture, attaches, eval 1+1
```

The integration test uses `tests\fixtures\sample-electron-app\`. First run
needs `npm install` in that directory plus its sibling `sample-addon\` (used
to populate napi exports for HELLO diagnostics).

## Third-party deps (vendored under `third_party\`)

| dep              | role                                       |
|------------------|--------------------------------------------|
| Blackbone        | manual DLL mapping into hardened processes |
| MinHook          | inline hook engine                         |
| nlohmann/json    | wire protocol JSON                         |
| CLI11            | host argument parsing                      |
| replxx           | host REPL                                  |
| doctest          | unit tests                                 |
