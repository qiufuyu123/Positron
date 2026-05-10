# positron SDK

Embed positron's Electron-injection capability into your own C++ program.
Stable C++ API surface; everything below `positron::sdk::` is supported.

## Adding to your project

**Includes**
```
$(SolutionDir)sdk/include
```

**Libraries** (link order matters for static-lib resolution)
```
positron_sdk.lib
BlackBone.lib       (in third_party/Blackbone/lib/$(Platform)/$(Configuration))
dbghelp.lib
Ws2_32.lib
```

**Compiler / linker constraints**
- C++20
- `/MTd` (Debug) or `/MT` (Release) — match the SDK's static-CRT build
- `Win32` for x86 Electron targets, `x64` for x64 — must match the target's
  bitness; positron does not cross-bitness inject

## API at a glance

```cpp
#include <positron/sdk.h>

// 1) Discover candidates
for (auto& p : positron::sdk::list_processes()) {
    std::wcout << p.pid << L'\t'
               << positron::sdk::type_name(p.type) << L'\t'
               << p.exe_name << L'\n';
}

// 2) Attach + eval
positron::sdk::Session s;
if (auto err = s.attach(target_pid)) {
    std::cerr << "attach failed: " << err->message << "\n"; return 1;
}

auto r = s.eval("process.versions.electron");
if (r.ok) std::cout << r.json_value << "\n";   // -> "39.0.0"

// 3) Hop into the renderer's main world (requires Electron *main* PID)
positron::sdk::EvalOptions opts;
opts.world        = positron::sdk::World::Renderer;
opts.window_index = 0;                          // BrowserWindow.getAllWindows()[0]
auto title = s.eval("document.title", opts);

// 4) Hooks (low-level: 4 register args at hit time)
s.on_hook_hit([](const positron::sdk::HookHit& h){
    std::cout << "hit on hook " << h.hook_id
              << " arg0=0x" << std::hex << h.args[0] << "\n";
});
uint64_t hid = s.install_hook("napi_throw");

// 5) Async eval (callback fires on a background thread)
s.eval_async("Date.now()", {}, [](positron::sdk::EvalResult r){
    std::cout << "now = " << r.json_value << "\n";
});

// 6) Cleanup
s.detach();   // disables hooks; closes the IPC channel.
              // The injected DLL stays mapped in the target until target exits.
```

## Threading model

- `Session` owns one TCP connection to the target's payload.
- A reader thread inside `Session` drains incoming frames and dispatches:
  - eval responses → wake a waiting `eval()` (or invoke its callback for `eval_async`)
  - hook.hit events → invoke `on_hook_hit` handler
- All public methods are thread-safe to call from any thread; just don't
  share a `Session` across attach/detach lifecycle boundaries (re-attach
  on the same instance is fine — `attach()` is idempotent).

## Bundling payload.dll

`Session::attach` defaults the payload path to `<dir-of-current-exe>/payload.dll`.
Either:

- ship `payload.dll` next to your application's `.exe`
- or pass an explicit path: `s.attach(pid, L"C:\\my\\payload.dll")`

`payload.dll` must match the bitness of the target process. Both arches
build out of `payload.vcxproj`; the static-lib SDK is bitness-agnostic
header-wise but you must build (sdk + your consumer + payload) all as
the same `Win32` or `x64`.

## Caveats

- **Sandbox**: Electron renderers running with the default
  `webPreferences.sandbox: true` block IPC server creation in-process,
  so direct attach to a renderer doesn't work. Use `World::Renderer`
  from a `main`-process attach — this hops via
  `webContents.executeJavaScript` and works around the sandbox.
- **`require` removed**: some apps strip global `require` for hardening.
  The SDK's renderer launcher falls back through
  `process.mainModule.require` and `process._linkedBinding` so this
  case is handled transparently.
- **V8 ABI**: positron resolves V8/libuv exports via fuzzy substring
  matching against the target binary's PE export table at attach time.
  This survives most version drift but if you hit a target where
  symbols are mangled differently (rare on Windows), you may need to
  extend `payload/src/v8_bridge/v8_bridge.cpp`.
