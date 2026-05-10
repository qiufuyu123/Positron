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

// =============================================================================
// Session
// =============================================================================

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
    // Returns nullopt on success; AttachError on failure.
    std::optional<AttachError> attach(uint32_t pid,
                                      const std::wstring& payload_dll = {});

    // Drop the connection and disable in-target hooks. The DLL stays mapped
    // in the target until the target exits (its trampolines may still be
    // mid-execution on other threads).
    void detach();

    bool is_connected() const;

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

private:
    struct Impl;
    std::unique_ptr<Impl> p;
};

} // namespace positron::sdk
