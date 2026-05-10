#include "pch.h"
#include "v8_bridge.h"
#include "pe_resolver/pe_resolver.h"
#include "logging/log.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// =============================================================================
// V8 + libuv FFI calling-convention notes (clang-cl built electron.exe / V8)
// =============================================================================
//
// STATIC functions returning a non-trivial type (Local<T>/MaybeLocal<T>):
//   args layout: [ret_ptr@RCX, args@RDX, R8, R9, stack...]
//   This matches the standard MSVC x64 ABI for non-trivial returns.
//
// MEMBER functions returning a non-trivial type:
//   args layout: [this@RCX, ret_ptr@RDX, args@R8, R9, stack...]
//   NOT standard MSVC ABI (which would put ret_ptr first). V8 (clang-cl per
//   Chromium build) uses Itanium-style this-first ordering for these. Verified
//   empirically on Electron 42 / V8 13.x.
//
// Trivial returns (pointer, bool, size_t) come back in RAX as usual.

// Storage for inline scope objects. Real V8 sizes on x64:
//   HandleScope ~= 24 bytes; TryCatch ~= 56 bytes; uv_async_t ~= 144 bytes.
// We over-allocate to cushion against V8 layout changes.
namespace { constexpr size_t kHandleScopeBytes = 64; }
namespace { constexpr size_t kTryCatchBytes    = 128; }
namespace { constexpr size_t kUvAsyncBytes     = 256; }

namespace {

// libuv (undecorated)
using p_uv_default_loop = void* (*)();
using p_uv_async_init   = int   (*)(void* loop, void* async, void(*cb)(void*));
using p_uv_async_send   = int   (*)(void* async);
using p_uv_unref        = void  (*)(void* handle);

// v8 — see ABI notes above
using p_Isolate_TryGetCurrent     = void* (*)();
using p_Isolate_GetCurrentContext = void  (*)(void* iso, void** ret);
using p_Isolate_InContext         = bool  (*)(void* iso);

using p_HandleScope_ctor = void (*)(void* hs_storage, void* iso);
using p_HandleScope_dtor = void (*)(void* hs_storage);

using p_String_NewFromUtf8 = void   (*)(void** ret, void* iso, const char* data, int new_string_type, int length);
using p_Script_Compile     = void   (*)(void** ret, void* ctx, void* src, void* origin);
using p_Script_Run         = void   (*)(void* script, void** ret, void* ctx);
using p_String_WriteUtf8V2 = size_t (*)(void* str, void* iso, char* buf, size_t cap, int flags, size_t* processed);

struct Resolved {
    p_uv_default_loop            uv_default_loop = nullptr;
    p_uv_async_init              uv_async_init   = nullptr;
    p_uv_async_send              uv_async_send   = nullptr;
    p_uv_unref                   uv_unref        = nullptr;

    p_Isolate_TryGetCurrent      Isolate_TryGetCurrent     = nullptr;
    p_Isolate_GetCurrentContext  Isolate_GetCurrentContext = nullptr;
    p_Isolate_InContext          Isolate_InContext         = nullptr;

    p_HandleScope_ctor           HandleScope_ctor = nullptr;
    p_HandleScope_dtor           HandleScope_dtor = nullptr;

    p_String_NewFromUtf8         String_NewFromUtf8 = nullptr;
    p_Script_Compile             Script_Compile     = nullptr;
    p_Script_Run                 Script_Run         = nullptr;
    p_String_WriteUtf8V2         String_WriteUtf8V2 = nullptr;
};

static Resolved g_r{};
static std::atomic<bool> g_ready{false};
static std::atomic<bool> g_init_attempted{false};

alignas(16) static uint8_t g_async_storage[kUvAsyncBytes];
static void* g_async_handle = static_cast<void*>(g_async_storage);

static std::mutex g_q_mu;
static std::queue<std::function<void()>> g_q;

static void async_cb(void* /*async*/) {
    std::queue<std::function<void()>> drain;
    {
        std::lock_guard<std::mutex> lk(g_q_mu);
        std::swap(drain, g_q);
    }
    while (!drain.empty()) {
        try { drain.front()(); }
        catch (...) { positron::log::error("v8_bridge: task threw"); }
        drain.pop();
    }
}

// Description of a single symbol to resolve: a pretty name for diagnostics
// plus the fuzzy SymbolPattern. The pattern is intentionally loose so it
// survives V8 ABI churn (extra template params, renamed inner classes, etc.)
// while still picking out exactly one symbol.
struct SymSpec {
    const char*                  pretty;
    void**                       slot;
    positron::pe::SymbolPattern  pattern;
};

// Build the table at runtime so the SymbolPattern initializer-lists are valid.
static std::vector<SymSpec> symbol_specs() {
    return {
        // libuv — exact substring match is enough since libuv exports are not mangled.
        {"uv_default_loop", reinterpret_cast<void**>(&g_r.uv_default_loop), {{"uv_default_loop"}}},
        {"uv_async_init",   reinterpret_cast<void**>(&g_r.uv_async_init),   {{"uv_async_init"}}},
        {"uv_async_send",   reinterpret_cast<void**>(&g_r.uv_async_send),   {{"uv_async_send"}}},
        {"uv_unref",        reinterpret_cast<void**>(&g_r.uv_unref),        {{"uv_unref"}}},

        // v8::Isolate::TryGetCurrent — fully qualified by class scope only;
        // unique within v8 namespace.
        {"Isolate::TryGetCurrent",
         reinterpret_cast<void**>(&g_r.Isolate_TryGetCurrent),
         {{"TryGetCurrent", "Isolate", "v8"}}},

        // v8::Isolate::GetCurrentContext — must contain GetCurrentContext +
        // Isolate@v8. Excludes "Internals" because there is also a similarly
        // named helper in v8::internal.
        {"Isolate::GetCurrentContext",
         reinterpret_cast<void**>(&g_r.Isolate_GetCurrentContext),
         {{"GetCurrentContext", "Isolate@v8"}, {"Internals"}}},

        // v8::Isolate::InContext
        {"Isolate::InContext",
         reinterpret_cast<void**>(&g_r.Isolate_InContext),
         {{"InContext", "Isolate@v8"}}},

        // HandleScope ctor taking Isolate*. ??0 = constructor in MSVC mangling.
        {"HandleScope::ctor(Isolate*)",
         reinterpret_cast<void**>(&g_r.HandleScope_ctor),
         {{"??0HandleScope@v8", "Isolate"}}},

        // HandleScope dtor. ??1 = destructor.
        {"HandleScope::dtor",
         reinterpret_cast<void**>(&g_r.HandleScope_dtor),
         {{"??1HandleScope@v8"}}},

        // v8::String::NewFromUtf8 — distinguished from sister overloads by the
        // NewStringType parameter substring.
        {"String::NewFromUtf8",
         reinterpret_cast<void**>(&g_r.String_NewFromUtf8),
         {{"NewFromUtf8", "String@v8", "NewStringType"}}},

        // v8::Script::Compile(Local<Context>, Local<String>, ScriptOrigin*)
        // — the ScriptOrigin parameter pins down the public 3-arg overload.
        {"Script::Compile",
         reinterpret_cast<void**>(&g_r.Script_Compile),
         {{"Compile@Script@v8", "ScriptOrigin"}}},

        // v8::Script::Run(Local<Context>) — there are two overloads. We want
        // the simpler one. Excluding "VData" rules out
        //   Run(Local<Context>, Local<Data>).
        {"Script::Run(Local<Context>)",
         reinterpret_cast<void**>(&g_r.Script_Run),
         {{"Run@Script@v8", "Local@VContext"}, {"VData"}}},

        // v8::String::WriteUtf8V2 — modern replacement for WriteUtf8.
        {"String::WriteUtf8V2",
         reinterpret_cast<void**>(&g_r.String_WriteUtf8V2),
         {{"WriteUtf8V2@String@v8"}}},
    };
}

} // anonymous namespace

namespace positron::v8b {

V8Bridge& V8Bridge::instance() {
    static V8Bridge b;
    return b;
}

bool V8Bridge::is_ready() const { return g_ready.load(std::memory_order_acquire); }

bool V8Bridge::initialize() {
    bool expected = false;
    if (!g_init_attempted.compare_exchange_strong(expected, true)) {
        return g_ready.load();
    }

    // Locate the module that exports V8 — we use a fuzzy marker rather than a
    // hard-coded mangled name so the bootstrap works across V8 versions whose
    // exact decoration may differ.
    auto found = pe::find_exports_matching({{"TryGetCurrent", "Isolate", "v8"}});
    if (!found) {
        log::warn("v8_bridge: no module exports a v8::Isolate::TryGetCurrent-like symbol");
        return false;
    }
    auto& t = found->table;

    auto specs = symbol_specs();
    bool all_ok = true;
    for (auto& spec : specs) {
        auto match = pe::find_one(t, spec.pattern);
        if (!match) {
            log::error(std::string{"v8_bridge: missing symbol: "} + spec.pretty);
            all_ok = false;
            continue;
        }
        *spec.slot = reinterpret_cast<void*>(match->abs_addr);
    }
    if (!all_ok) return false;

    void* loop = g_r.uv_default_loop();
    if (!loop) {
        log::warn("v8_bridge: uv_default_loop returned nullptr");
        return false;
    }
    std::memset(g_async_storage, 0, sizeof(g_async_storage));
    int rc = g_r.uv_async_init(loop, g_async_handle, &async_cb);
    if (rc != 0) {
        log::error("v8_bridge: uv_async_init failed rc=" + std::to_string(rc));
        return false;
    }
    if (g_r.uv_unref) g_r.uv_unref(g_async_handle);

    g_ready.store(true, std::memory_order_release);
    log::info("v8_bridge: ready (resolved " + std::to_string(specs.size())
              + " symbols, armed async on default loop)");
    return true;
}

bool V8Bridge::run_on_v8_thread(std::function<void()> fn) {
    if (!g_ready.load(std::memory_order_acquire)) return false;
    {
        std::lock_guard<std::mutex> lk(g_q_mu);
        g_q.push(std::move(fn));
    }
    g_r.uv_async_send(g_async_handle);
    return true;
}

bool V8Bridge::eval_async(std::string code, std::function<void(EvalOutcome)> done) {
    if (!g_ready.load(std::memory_order_acquire)) return false;

    // Wrap the user code so the result is always a JSON string. This keeps the
    // C-side serialisation trivial: just read a UTF-8 string out of V8.
    using nlohmann::json;
    std::string js_string_literal = json(code).dump();
    std::string wrapped =
        std::string{"(function(__src){try{var v=(0,eval)(__src);return JSON.stringify({type:typeof v,value:v})}catch(e){return JSON.stringify({error:{message:String((e&&e.message)||e),stack:String((e&&e.stack)||\"\")}})}})("} +
        js_string_literal + ")";

    return run_on_v8_thread([wrapped = std::move(wrapped), done = std::move(done)]() mutable {
        EvalOutcome out{};
        out.ok = false;

        void* iso = g_r.Isolate_TryGetCurrent();
        if (!iso) {
            out.error_message = "v8_bridge: no isolate on this thread";
            done(std::move(out));
            return;
        }

        alignas(16) uint8_t hs_buf[kHandleScopeBytes];
        std::memset(hs_buf, 0, sizeof(hs_buf));
        g_r.HandleScope_ctor(hs_buf, iso);

        // Compile/Run accept Local<Context> directly — no need to enter it.
        void* ctx = nullptr;
        g_r.Isolate_GetCurrentContext(iso, &ctx);
        if (!ctx) {
            g_r.HandleScope_dtor(hs_buf);
            out.error_message = "v8_bridge: no current context";
            done(std::move(out));
            return;
        }

        void* src = nullptr;
        g_r.String_NewFromUtf8(&src, iso, wrapped.c_str(), 0, static_cast<int>(wrapped.size()));
        if (!src) {
            g_r.HandleScope_dtor(hs_buf);
            out.error_message = "v8_bridge: NewFromUtf8 failed";
            done(std::move(out));
            return;
        }

        void* script = nullptr;
        g_r.Script_Compile(&script, ctx, src, nullptr);
        if (!script) {
            g_r.HandleScope_dtor(hs_buf);
            out.error_message = "v8_bridge: compile failed";
            done(std::move(out));
            return;
        }

        void* result = nullptr;
        g_r.Script_Run(script, &result, ctx);
        if (!result) {
            g_r.HandleScope_dtor(hs_buf);
            out.error_message = "v8_bridge: run failed";
            done(std::move(out));
            return;
        }

        // The wrapper guarantees the result is a JS string we can read directly.
        char small[4096];
        size_t processed = 0;
        size_t n = g_r.String_WriteUtf8V2(result, iso, small, sizeof(small) - 1, 0, &processed);
        std::string s(small, small + n);
        g_r.HandleScope_dtor(hs_buf);

        try {
            auto j = nlohmann::json::parse(s);
            if (j.contains("error")) {
                out.ok = false;
                out.error_message = j["error"].value("message", std::string{});
                out.error_stack   = j["error"].value("stack",   std::string{});
            } else {
                out.ok = true;
                out.type_tag = j.value("type", std::string{});
                if (j.contains("value") && !j["value"].is_null()) {
                    out.json_value = j["value"].dump();
                } else {
                    out.json_value = "null";
                }
            }
        } catch (const std::exception& e) {
            out.ok = false;
            out.error_message = std::string{"v8_bridge: result parse error: "} + e.what();
            out.error_stack = s;
        }

        done(std::move(out));
    });
}

} // namespace positron::v8b
