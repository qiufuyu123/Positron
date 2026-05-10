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
// FFI: opaque types and function-pointer typedefs
// =============================================================================
//
// Win x64 ABI: all calls use the same calling convention. Member functions
// receive `this` in RCX, then args in RDX/R8/R9/stack. Return values <=8 bytes
// go in RAX. v8::Local<T> and v8::MaybeLocal<T> are 8 bytes (single pointer);
// passed as void* and returned in RAX. nullptr means an "empty" MaybeLocal.

// Marker symbol we look for to identify which loaded module hosts V8.
// This mangled name is the export of `v8::Isolate::TryGetCurrent` on MSVC x64.
static constexpr const char kV8MarkerSymbol[] =
    "?TryGetCurrent@Isolate@v8@@SAPEAV12@XZ";

namespace {

// libuv
using p_uv_default_loop = void* (*)();
using p_uv_async_init   = int   (*)(void* loop, void* async, void(*cb)(void*));
using p_uv_async_send   = int   (*)(void* async);
using p_uv_unref        = void  (*)(void* handle);

// MSVC x64 ABI: types with non-trivial copy ctor/dtor (which v8::Local<T> /
// MaybeLocal<T> have, due to user-defined ctors) are returned via a hidden
// pointer in RCX, with `this` shifting to RDX. We model this as an output
// parameter `void** ret`. Trivial returns (Isolate*, size_t, bool) stay in RAX.

// Calling convention notes for V8 (clang-cl built):
//
// STATIC functions returning non-trivial types (Local<T>/MaybeLocal<T>):
//   [ret_ptr@RCX, args@RDX,R8,R9,...]
//   Matches standard MSVC x64 ABI for non-trivial returns.
//
// MEMBER functions returning non-trivial types:
//   [this@RCX, ret_ptr@RDX, args@R8,R9,...]
//   This is NOT standard MSVC ABI (which would put ret_ptr first). V8 (clang-cl
//   built per Chromium build conventions) uses this Itanium-style ordering.
//   Determined empirically against Electron 42 / V8 13.x.

// Isolate* Isolate::TryGetCurrent()  (static; trivial pointer return -> RAX)
using p_Isolate_TryGetCurrent = void* (*)();

// Local<Context> Isolate::GetCurrentContext()  (member; non-trivial return)
using p_Isolate_GetCurrentContext = void (*)(void* iso, void** ret);

// bool Isolate::InContext()  (member; bool in RAX)
using p_Isolate_InContext = bool (*)(void* iso);

// Local<Context> Isolate::GetEnteredOrMicrotaskContext()  (member; non-trivial return)
using p_Isolate_GetEnteredOrMicrotaskContext = void (*)(void* iso, void** ret);
// Local<Context> Isolate::GetIncumbentContext()  (member; non-trivial return)
using p_Isolate_GetIncumbentContext = void (*)(void* iso, void** ret);

// HandleScope ctor/dtor — return void
using p_HandleScope_ctor = void (*)(void* hs_storage, void* iso);
using p_HandleScope_dtor = void (*)(void* hs_storage);
using p_TryCatch_ctor    = void (*)(void* tc_storage, void* iso);
using p_TryCatch_dtor    = void (*)(void* tc_storage);

// Context::Enter / Exit (member, return void)
using p_Context_Enter = void (*)(void* ctx);
using p_Context_Exit  = void (*)(void* ctx);

// MaybeLocal<String> String::NewFromUtf8(...)  (static; non-trivial return)
using p_String_NewFromUtf8 = void (*)(void** ret, void* iso, const char* data, int new_string_type, int length);

// MaybeLocal<Script> Script::Compile(...)  (static; non-trivial return)
using p_Script_Compile = void (*)(void** ret, void* ctx, void* src, void* origin);

// MaybeLocal<Value> Script::Run(Local<Context>)  (member; non-trivial return)
using p_Script_Run = void (*)(void* script, void** ret, void* ctx);

// size_t String::WriteUtf8V2(...)  (member; size_t in RAX)
using p_String_WriteUtf8V2 = size_t (*)(void* str, void* iso, char* buf, size_t cap, int flags, size_t* processed);

// MaybeLocal<String> Value::ToString(Local<Context>) const  (member; non-trivial return)
using p_Value_ToString = void (*)(void* val, void** ret, void* ctx);

// bool TryCatch::HasCaught() const  (member; bool in RAX)
using p_TryCatch_HasCaught = bool (*)(const void* tc);
// Local<Value> TryCatch::Exception() const  (member; non-trivial return)
using p_TryCatch_Exception = void (*)(const void* tc, void** ret);

// Storage layout for inline scope objects.
// Worst-case sizing — V8 internal layout fits comfortably here.
constexpr size_t kHandleScopeBytes = 64;
constexpr size_t kTryCatchBytes    = 128;
constexpr size_t kUvAsyncBytes     = 256;

struct Resolved {
    p_uv_default_loop          uv_default_loop = nullptr;
    p_uv_async_init            uv_async_init   = nullptr;
    p_uv_async_send            uv_async_send   = nullptr;
    p_uv_unref                 uv_unref        = nullptr;

    p_Isolate_TryGetCurrent     Isolate_TryGetCurrent     = nullptr;
    p_Isolate_GetCurrentContext Isolate_GetCurrentContext = nullptr;
    p_Isolate_InContext         Isolate_InContext         = nullptr;
    p_Isolate_GetEnteredOrMicrotaskContext Isolate_GetEnteredOrMicrotaskContext = nullptr;
    p_Isolate_GetIncumbentContext Isolate_GetIncumbentContext = nullptr;

    p_HandleScope_ctor          HandleScope_ctor = nullptr;
    p_HandleScope_dtor          HandleScope_dtor = nullptr;
    p_TryCatch_ctor             TryCatch_ctor    = nullptr;
    p_TryCatch_dtor             TryCatch_dtor    = nullptr;

    p_Context_Enter             Context_Enter   = nullptr;
    p_Context_Exit              Context_Exit    = nullptr;

    p_String_NewFromUtf8        String_NewFromUtf8 = nullptr;
    p_Script_Compile            Script_Compile     = nullptr;
    p_Script_Run                Script_Run         = nullptr;
    p_Value_ToString            Value_ToString     = nullptr;
    p_String_WriteUtf8V2        String_WriteUtf8V2 = nullptr;

    p_TryCatch_HasCaught        TryCatch_HasCaught = nullptr;
    p_TryCatch_Exception        TryCatch_Exception = nullptr;

    bool all_present() const {
        return uv_default_loop && uv_async_init && uv_async_send
            && Isolate_TryGetCurrent && Isolate_GetCurrentContext
            && HandleScope_ctor && HandleScope_dtor
            && TryCatch_ctor && TryCatch_dtor
            && Context_Enter && Context_Exit
            && String_NewFromUtf8 && Script_Compile && Script_Run
            && Value_ToString && String_WriteUtf8V2
            && TryCatch_HasCaught && TryCatch_Exception;
    }
};

static Resolved g_r{};
static std::atomic<bool> g_ready{false};
static std::atomic<bool> g_init_attempted{false};

// uv_async_t storage; written once during init then never moved.
alignas(16) static uint8_t g_async_storage[kUvAsyncBytes];
static void* g_async_handle = static_cast<void*>(g_async_storage);

static std::mutex g_q_mu;
static std::queue<std::function<void()>> g_q;

static void async_cb(void* /*async*/) {
    positron::log::info("v8_bridge: async_cb fired");
    std::queue<std::function<void()>> drain;
    {
        std::lock_guard<std::mutex> lk(g_q_mu);
        std::swap(drain, g_q);
    }
    positron::log::info("v8_bridge: draining " + std::to_string(drain.size()) + " task(s)");
    while (!drain.empty()) {
        try { drain.front()(); }
        catch (...) { positron::log::error("v8_bridge: task threw"); }
        drain.pop();
    }
}

static void* resolve_in(positron::pe::ExportTable& t, const char* name) {
    auto a = t.abs(name);
    return a ? reinterpret_cast<void*>(*a) : nullptr;
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

    auto found = pe::find_exports_with(kV8MarkerSymbol);
    if (!found) {
        log::warn("v8_bridge: V8 marker symbol not found in any candidate module");
        return false;
    }
    auto& t = found->table;

    g_r.uv_default_loop          = (p_uv_default_loop)          resolve_in(t, "uv_default_loop");
    g_r.uv_async_init            = (p_uv_async_init)            resolve_in(t, "uv_async_init");
    g_r.uv_async_send            = (p_uv_async_send)            resolve_in(t, "uv_async_send");
    g_r.uv_unref                 = (p_uv_unref)                 resolve_in(t, "uv_unref");

    g_r.Isolate_TryGetCurrent     = (p_Isolate_TryGetCurrent)     resolve_in(t, "?TryGetCurrent@Isolate@v8@@SAPEAV12@XZ");
    g_r.Isolate_GetCurrentContext = (p_Isolate_GetCurrentContext) resolve_in(t, "?GetCurrentContext@Isolate@v8@@QEAA?AV?$Local@VContext@v8@@@2@XZ");
    g_r.Isolate_InContext         = (p_Isolate_InContext)         resolve_in(t, "?InContext@Isolate@v8@@QEAA_NXZ");
    g_r.Isolate_GetEnteredOrMicrotaskContext = (p_Isolate_GetEnteredOrMicrotaskContext) resolve_in(t, "?GetEnteredOrMicrotaskContext@Isolate@v8@@QEAA?AV?$Local@VContext@v8@@@2@XZ");
    g_r.Isolate_GetIncumbentContext = (p_Isolate_GetIncumbentContext) resolve_in(t, "?GetIncumbentContext@Isolate@v8@@QEAA?AV?$Local@VContext@v8@@@2@XZ");

    g_r.HandleScope_ctor = (p_HandleScope_ctor) resolve_in(t, "??0HandleScope@v8@@QEAA@PEAVIsolate@1@@Z");
    g_r.HandleScope_dtor = (p_HandleScope_dtor) resolve_in(t, "??1HandleScope@v8@@QEAA@XZ");
    g_r.TryCatch_ctor    = (p_TryCatch_ctor)    resolve_in(t, "??0TryCatch@v8@@QEAA@PEAVIsolate@1@@Z");
    g_r.TryCatch_dtor    = (p_TryCatch_dtor)    resolve_in(t, "??1TryCatch@v8@@QEAA@XZ");

    g_r.Context_Enter = (p_Context_Enter) resolve_in(t, "?Enter@Context@v8@@QEAAXXZ");
    g_r.Context_Exit  = (p_Context_Exit)  resolve_in(t, "?Exit@Context@v8@@QEAAXXZ");

    g_r.String_NewFromUtf8 = (p_String_NewFromUtf8) resolve_in(t,
        "?NewFromUtf8@String@v8@@SA?AV?$MaybeLocal@VString@v8@@@2@PEAVIsolate@2@PEBDW4NewStringType@2@H@Z");
    g_r.Script_Compile = (p_Script_Compile) resolve_in(t,
        "?Compile@Script@v8@@SA?AV?$MaybeLocal@VScript@v8@@@2@V?$Local@VContext@v8@@@2@V?$Local@VString@v8@@@2@PEAVScriptOrigin@2@@Z");
    g_r.Script_Run = (p_Script_Run) resolve_in(t,
        "?Run@Script@v8@@QEAA?AV?$MaybeLocal@VValue@v8@@@2@V?$Local@VContext@v8@@@2@@Z");
    g_r.Value_ToString = (p_Value_ToString) resolve_in(t,
        "?ToString@Value@v8@@QEBA?AV?$MaybeLocal@VString@v8@@@2@V?$Local@VContext@v8@@@2@@Z");
    g_r.String_WriteUtf8V2 = (p_String_WriteUtf8V2) resolve_in(t,
        "?WriteUtf8V2@String@v8@@QEBA_KPEAVIsolate@2@PEAD_KHPEA_K@Z");

    g_r.TryCatch_HasCaught = (p_TryCatch_HasCaught) resolve_in(t, "?HasCaught@TryCatch@v8@@QEBA_NXZ");
    g_r.TryCatch_Exception = (p_TryCatch_Exception) resolve_in(t, "?Exception@TryCatch@v8@@QEBA?AV?$Local@VValue@v8@@@2@XZ");

    if (!g_r.all_present()) {
        log::warn("v8_bridge: not all required symbols resolved");
        return false;
    }

    log::info("v8_bridge syms: TryGetCurrent=" + std::to_string(reinterpret_cast<uintptr_t>(g_r.Isolate_TryGetCurrent))
              + " GetCurrentContext=" + std::to_string(reinterpret_cast<uintptr_t>(g_r.Isolate_GetCurrentContext))
              + " HandleScope_ctor=" + std::to_string(reinterpret_cast<uintptr_t>(g_r.HandleScope_ctor))
              + " NewFromUtf8=" + std::to_string(reinterpret_cast<uintptr_t>(g_r.String_NewFromUtf8))
              + " ScriptCompile=" + std::to_string(reinterpret_cast<uintptr_t>(g_r.Script_Compile))
              + " ScriptRun=" + std::to_string(reinterpret_cast<uintptr_t>(g_r.Script_Run)));

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
    log::info("v8_bridge: ready (async armed on default loop)");
    return true;
}

bool V8Bridge::run_on_v8_thread(std::function<void()> fn) {
    if (!g_ready.load(std::memory_order_acquire)) return false;
    {
        std::lock_guard<std::mutex> lk(g_q_mu);
        g_q.push(std::move(fn));
    }
    int rc = g_r.uv_async_send(g_async_handle);
    log::info("v8_bridge: uv_async_send rc=" + std::to_string(rc));
    return true;
}

bool V8Bridge::eval_async(std::string code, std::function<void(EvalOutcome)> done) {
    if (!g_ready.load(std::memory_order_acquire)) return false;

    // Wrap user code so the result is always a JSON string, even on throw.
    // The wrapper preserves typeof via a {type, value} envelope.
    using nlohmann::json;
    std::string js_string_literal = json(code).dump(); // produces e.g. "1+1" with quotes/escapes
    std::string wrapped =
        std::string{"(function(__src){try{var v=(0,eval)(__src);return JSON.stringify({type:typeof v,value:v})}catch(e){return JSON.stringify({error:{message:String((e&&e.message)||e),stack:String((e&&e.stack)||\"\")}})}})("} +
        js_string_literal + ")";

    return run_on_v8_thread([wrapped = std::move(wrapped), done = std::move(done)]() mutable {
        log::info("eval task: enter");
        EvalOutcome out{};
        out.ok = false;

        void* iso = g_r.Isolate_TryGetCurrent();
        log::info("eval task: TryGetCurrent -> " + std::to_string(reinterpret_cast<uintptr_t>(iso)));
        if (!iso) {
            out.error_message = "v8_bridge: no isolate on this thread";
            done(std::move(out));
            return;
        }

        // HandleScope first
        alignas(16) uint8_t hs_buf[kHandleScopeBytes];
        std::memset(hs_buf, 0, sizeof(hs_buf));
        log::info("eval task: HandleScope ctor");
        g_r.HandleScope_ctor(hs_buf, iso);
        log::info("eval task: HandleScope ok");

        // (debug) is a context currently entered?
        if (g_r.Isolate_InContext) {
            log::info("eval task: InContext=" + std::to_string(g_r.Isolate_InContext(iso)));
        }

        // Compile/Run accept Local<Context> as a parameter — no need to enter it.
        void* ctx = nullptr;
        g_r.Isolate_GetCurrentContext(iso, &ctx);
        log::info("eval task: GetCurrentContext -> " + std::to_string(reinterpret_cast<uintptr_t>(ctx)));
        if (!ctx) {
            g_r.HandleScope_dtor(hs_buf);
            out.error_message = "v8_bridge: no current context";
            done(std::move(out));
            return;
        }

        log::info("eval task: NewFromUtf8 (len=" + std::to_string(wrapped.size()) + ")");
        void* src = nullptr;
        g_r.String_NewFromUtf8(&src, iso, wrapped.c_str(), 0, static_cast<int>(wrapped.size()));
        log::info("eval task: NewFromUtf8 -> " + std::to_string(reinterpret_cast<uintptr_t>(src)));
        if (!src) {
            g_r.HandleScope_dtor(hs_buf);
            out.error_message = "v8_bridge: NewFromUtf8 failed";
            done(std::move(out));
            return;
        }

        log::info("eval task: Script::Compile");
        void* script = nullptr;
        g_r.Script_Compile(&script, ctx, src, nullptr);
        log::info("eval task: Script::Compile -> " + std::to_string(reinterpret_cast<uintptr_t>(script)));
        if (!script) {
            g_r.HandleScope_dtor(hs_buf);
            out.error_message = "v8_bridge: compile failed";
            done(std::move(out));
            return;
        }

        log::info("eval task: Script::Run");
        void* result = nullptr;
        g_r.Script_Run(script, &result, ctx);
        log::info("eval task: Script::Run -> " + std::to_string(reinterpret_cast<uintptr_t>(result)));
        if (!result) {
            g_r.HandleScope_dtor(hs_buf);
            out.error_message = "v8_bridge: run failed";
            done(std::move(out));
            return;
        }

        log::info("eval task: WriteUtf8V2");
        // result is the JS string returned by our wrapper.
        char small[4096];
        size_t processed = 0;
        size_t n = g_r.String_WriteUtf8V2(result, iso, small, sizeof(small) - 1, 0, &processed);
        std::string s(small, small + n);
        log::info("eval task: result string len=" + std::to_string(n));

        g_r.HandleScope_dtor(hs_buf);

        // Parse the JSON wrapper to extract type/value/error.
        try {
            auto j = nlohmann::json::parse(s);
            if (j.contains("error")) {
                out.ok = false;
                out.error_message = j["error"].value("message", std::string{});
                out.error_stack   = j["error"].value("stack",   std::string{});
            } else {
                out.ok = true;
                out.type_tag   = j.value("type", std::string{});
                if (j.contains("value") && !j["value"].is_null()) {
                    out.json_value = j["value"].dump();
                } else {
                    out.json_value = "null";
                }
            }
        } catch (const std::exception& e) {
            out.ok = false;
            out.error_message = std::string{"v8_bridge: result parse error: "} + e.what();
            out.error_stack   = s;
        }

        log::info("eval task: done ok=" + std::to_string(out.ok));
        done(std::move(out));
    });
}

} // namespace positron::v8b
