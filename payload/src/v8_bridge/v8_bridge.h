#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace positron::v8b {

// V8Bridge — directly resolves V8 + libuv symbols (mangled and undecorated)
// from the host module's export table. No napi addon required: we use
// uv_default_loop / uv_async_send to enter the V8 thread, then call
// v8::Isolate::TryGetCurrent + v8::Script::Compile/Run via mangled exports.
//
// All public methods are thread-safe except run_on_v8_thread's callback,
// which must observe its own threading rules (it runs on the V8 thread).
struct V8Bridge {
    static V8Bridge& instance();

    // Resolve symbols and arm the cross-thread async handle.
    // Returns true iff every required symbol was found AND the uv_async
    // handle was registered with the default loop.
    bool initialize();

    bool is_ready() const;

    // Schedule fn to run on the V8 thread. Returns false if not ready.
    bool run_on_v8_thread(std::function<void()> fn);

    struct EvalOutcome {
        bool ok;
        std::string type_tag;        // typeof of the result (e.g. "number", "object")
        std::string json_value;      // JSON.stringify of result (best-effort)
        std::string error_message;   // populated when ok=false
        std::string error_stack;
    };

    // Schedule eval; `done` is invoked on V8 thread with the outcome.
    // Returns false if bridge is not ready.
    bool eval_async(std::string code, std::function<void(EvalOutcome)> done);

    // Hop into a renderer's main world via webContents.executeJavaScript and
    // run `code` there. Must be invoked against an Electron MAIN process (the
    // one that owns BrowserWindow). `window_index` selects which window when
    // multiple are open. Resolution is asynchronous (a Mojo IPC round-trip);
    // `done` fires when the renderer-side promise settles or the per-eval
    // deadline expires.
    bool eval_renderer_async(std::string code,
                             int window_index,
                             std::function<void(EvalOutcome)> done);
};

} // namespace positron::v8b
