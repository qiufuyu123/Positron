#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace positron::sdk {

// =============================================================================
// Eval
// =============================================================================

enum class World {
    Auto,       // run directly in the target's current V8 context
    Node,       // alias for Auto (kept for clarity in main-process code)
    Renderer,   // hop into BrowserWindow[window_index]'s main world via
                // webContents.executeJavaScript (target must be the Electron
                // MAIN process). Result is awaited transparently.
};

struct EvalOptions {
    World    world        = World::Auto;
    int      window_index = 0;       // for World::Renderer
    uint32_t timeout_ms   = 60000;
};

struct EvalResult {
    bool        ok = false;
    std::string type_tag;            // typeof of the JS value: "number" / "string" / "object" / ...
    std::string json_value;          // JSON.stringify of the value (raw, you can re-parse it)
    std::string error_message;
    std::string error_stack;
};

// =============================================================================
// Hooks
// =============================================================================

struct HookHit {
    uint64_t    hook_id;
    // First four register-passed arguments at hook fire time (RCX/RDX/R8/R9
    // on x64; ECX/EDX/EAX on x86 thiscall — interpretation depends on the
    // hooked function's signature).
    std::vector<uint64_t> args;
    // Number of hits the SPSC ring dropped between the previous report and
    // this one (busy hooks may overflow).
    uint32_t    dropped_since_last = 0;
};

using HookHitHandler = std::function<void(const HookHit&)>;

// Unsolicited frames the bootstrap pushes to host (log, mod.event, etc.)
struct Message {
    std::string kind;     // "log", "mod.event", ...
    std::string json;     // raw JSON of the entire frame
};
using MessageHandler = std::function<void(const Message&)>;

// =============================================================================
// Session
// =============================================================================

// Transport mode after attach negotiates with the payload.
//
//   V2     (default) — payload evaluates a JS bootstrap that takes over the
//                      transport. The SDK then talks to the JS server (in
//                      the target's main V8 isolate) directly. Native hooks
//                      are NOT available in this mode; install_hook() will
//                      return 0 and emit a warning. Pro: clean async
//                      semantics, no microtask-checkpoint complexity, host
//                      can detach + reconnect without re-injecting.
//   Native           — payload's C++ ipc_server stays in charge, same as
//                      pre-v2 behavior. Required for native-symbol hooks
//                      (.hook <export>) and for raw V8 ABI access.
enum class Transport { V2, Native };

class Session {
public:
    Session();
    ~Session();

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;

    struct AttachError { std::string message; };

    // Inject the payload into a target process and establish IPC.
    //   pid          target process ID
    //   payload_dll  path to positron payload DLL; pass empty to use
    //                "<dir-of-current-exe>/payload.dll".
    //   transport    V2 (default) or Native. See Transport doc above.
    // Returns nullopt on success; AttachError on failure.
    std::optional<AttachError> attach(uint32_t pid,
                                      const std::wstring& payload_dll = {},
                                      Transport transport = Transport::V2);

    // Drop the connection and disable in-target hooks. The DLL stays mapped
    // in the target until the target exits (its trampolines may still be
    // mid-execution on other threads).
    void detach();

    bool is_connected() const;

    // Which transport this session ended up on (only meaningful while
    // is_connected() is true).
    Transport transport() const;

    // Address at which the payload DLL was manually mapped during attach,
    // or 0 if attach hasn't run / failed. Useful for verifying phase-2
    // self-unmap (caller can VirtualQueryEx this to check MEM_FREE).
    uint64_t injected_module_base() const;

    // Returns true if a VirtualQueryEx on the injected base reports the
    // region is MEM_FREE (i.e. payload self-unmapped). Returns false if
    // the page is still committed, or if the query fails. Intended for
    // V2 mode after bootstrap completes.
    bool verify_payload_unmapped();

    // Synchronously evaluate JS in the target. Blocks the caller for up to
    // opts.timeout_ms.
    EvalResult eval(const std::string& code, const EvalOptions& opts = {});

    // Same, but returns immediately. `cb` is invoked from a background
    // thread when the result arrives (or on timeout).
    void eval_async(const std::string& code,
                    const EvalOptions& opts,
                    std::function<void(EvalResult)> cb);

    // Install a runtime hook on a target export. Returns the assigned hook id
    // (>0) on success, or 0 if the symbol couldn't be resolved.
    // Hits arrive via on_hook_hit callbacks.
    uint64_t install_hook(const std::string& symbol);

    // Subscribe to hook.hit events. Replaces any prior handler.
    void on_hook_hit(HookHitHandler handler);

    // Subscribe to unsolicited messages (log, mod.event, etc.) from the
    // bootstrap JS server. Replaces any prior handler.
    void on_message(MessageHandler handler);

private:
    struct Impl;
    std::unique_ptr<Impl> p;
};

} // namespace positron::sdk
