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
// V8 + libuv FFI calling-convention notes
// =============================================================================
//
// x64 (clang-cl built V8):
//   STATIC fn, non-trivial return:  [ret_ptr@RCX, args@RDX, R8, R9, stack...]
//     — matches standard MSVC x64.
//   MEMBER fn, non-trivial return:  [this@RCX, ret_ptr@RDX, args@R8, R9, ...]
//     — Itanium-style this-first; NOT standard MSVC. Verified on Electron 42
//       / V8 13.x.
//   Trivial returns (pointer/bool/size_t) → RAX.
//
// x86 (Win32):
//   STATIC fn (cdecl), non-trivial return:  [ret_ptr, args...] all on stack.
//   MEMBER fn (thiscall), non-trivial return:
//     this in ECX; ret_ptr is the first stack slot; args follow.
//   Trivial returns → EAX.
//
// Use CC and CCM macros below to write one set of typedefs that compile to
// the right calling convention on both architectures.

#ifdef _M_IX86
  #define POS_CC   __cdecl
  #define POS_CCM  __thiscall
#else
  #define POS_CC
  #define POS_CCM
#endif

// Storage for inline scope objects. Real V8/uv sizes vary across versions
// and bitness; we over-allocate generously. (uv_async_t in particular is a
// large libuv handle; 256B fits comfortably on x86 and x64 alike.)
namespace { constexpr size_t kHandleScopeBytes = 64; }
namespace { constexpr size_t kTryCatchBytes    = 128; }
namespace { constexpr size_t kUvAsyncBytes     = 256; }

namespace {

// libuv (undecorated; cdecl on both archs)
using p_uv_default_loop = void* (POS_CC *)();
using p_uv_async_init   = int   (POS_CC *)(void* loop, void* async, void(POS_CC *cb)(void*));
using p_uv_async_send   = int   (POS_CC *)(void* async);
using p_uv_unref        = void  (POS_CC *)(void* handle);

// v8 statics (cdecl) — see ABI notes above
using p_Isolate_TryGetCurrent     = void* (POS_CC *)();

// v8 members (thiscall on x86, default on x64)
using p_Isolate_GetCurrentContext = void  (POS_CCM *)(void* iso, void** ret);
using p_Isolate_InContext         = bool  (POS_CCM *)(void* iso);
using p_Isolate_PerformMicrotaskCheckpoint = void (POS_CCM *)(void* iso);

using p_HandleScope_ctor = void (POS_CCM *)(void* hs_storage, void* iso);
using p_HandleScope_dtor = void (POS_CCM *)(void* hs_storage);

// statics (cdecl)
using p_String_NewFromUtf8 = void   (POS_CC *)(void** ret, void* iso, const char* data, int new_string_type, int length);
using p_Script_Compile     = void   (POS_CC *)(void** ret, void* ctx, void* src, void* origin);

// members
using p_Script_Run         = void   (POS_CCM *)(void* script, void** ret, void* ctx);
using p_String_WriteUtf8V2 = size_t (POS_CCM *)(void* str, void* iso, char* buf, size_t cap, int flags, size_t* processed);

struct Resolved {
    p_uv_default_loop            uv_default_loop = nullptr;
    p_uv_async_init              uv_async_init   = nullptr;
    p_uv_async_send              uv_async_send   = nullptr;
    p_uv_unref                   uv_unref        = nullptr;

    p_Isolate_TryGetCurrent      Isolate_TryGetCurrent     = nullptr;
    p_Isolate_GetCurrentContext  Isolate_GetCurrentContext = nullptr;
    p_Isolate_InContext          Isolate_InContext         = nullptr;
    p_Isolate_PerformMicrotaskCheckpoint Isolate_PerformMicrotaskCheckpoint = nullptr;

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

static void POS_CC async_cb(void* /*async*/) {
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

        // v8::Isolate::PerformMicrotaskCheckpoint — drains queued microtasks.
        // Required after we Compile/Run code that schedules .then() handlers
        // on Promises returned by webContents.executeJavaScript, in case the
        // host's microtasks policy is kExplicit / kScoped (Node default is
        // kAuto, but Electron embeddings may differ).
        {"Isolate::PerformMicrotaskCheckpoint",
         reinterpret_cast<void**>(&g_r.Isolate_PerformMicrotaskCheckpoint),
         {{"PerformMicrotaskCheckpoint", "Isolate", "v8"}}},
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

} // namespace positron::v8b

// =============================================================================
// V8-thread helpers (callers MUST be on the V8 thread)
// =============================================================================

namespace {

// Compile + run a JS string. The script SHOULD return a JS string; this is
// extracted via WriteUtf8V2 into `out_string`. On failure, returns false and
// sets `out_err`. (Caller owns HandleScope.)
bool run_script_get_string(void* iso, void* ctx, const std::string& js,
                           std::string& out_string, std::string& out_err) {
    void* src = nullptr;
    g_r.String_NewFromUtf8(&src, iso, js.c_str(), 0, static_cast<int>(js.size()));
    if (!src) { out_err = "NewFromUtf8 failed"; return false; }

    void* script = nullptr;
    g_r.Script_Compile(&script, ctx, src, nullptr);
    if (!script) { out_err = "compile failed"; return false; }

    void* result = nullptr;
    g_r.Script_Run(script, &result, ctx);
    if (!result) { out_err = "run failed"; return false; }

    // Buffer sized for typical eval results; renderer hop wrappers read just
    // a single small JSON object from the launcher script (which doesn't
    // return anything), so 4 KB is plenty here.
    char small[8192];
    size_t processed = 0;
    size_t n = g_r.String_WriteUtf8V2(result, iso, small, sizeof(small) - 1, 0, &processed);
    out_string.assign(small, small + n);
    return true;
}

// Parse a JSON wrapper-string of shape:
//   {"type": "...", "value": ...}        — ok
//   {"error": {"message": "...", "stack": "..."}}   — failure
// Mutates `out` accordingly.
void apply_wrapper_json(const std::string& s, positron::v8b::V8Bridge::EvalOutcome& out) {
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
        out.error_message = std::string{"result parse error: "} + e.what();
        out.error_stack = s;
    }
}

// =============================================================================
// Renderer-eval pending list + poller
// =============================================================================
//
// webContents.executeJavaScript() returns a Promise that resolves only after a
// Mojo IPC round-trip with the renderer process. We can't block the main V8
// thread waiting for that — V8 itself has to keep running to even receive the
// IPC. So we:
//
//   1. run a "launcher" script that schedules a .then() handler which, on
//      resolution, parks the result on globalThis under a unique key
//      "_pR<id>";
//   2. push a PendingRenderer entry onto g_renderer_pending;
//   3. let a low-frequency poller thread schedule a single batched read on
//      the V8 thread every ~100 ms — that read pulls every resolved key in
//      one Compile/Run, so the cost stays O(1) regardless of how many evals
//      are in flight.

struct PendingRenderer {
    uint64_t                                  id;
    std::function<void(positron::v8b::V8Bridge::EvalOutcome)> done;
    std::chrono::steady_clock::time_point     deadline;
};

static std::mutex                  g_renderer_mu;
static std::vector<PendingRenderer> g_renderer_pending;
static std::atomic<uint64_t>       g_renderer_id_counter{1};
static std::atomic<bool>           g_poller_started{false};

void poll_renderer_pending_on_v8_thread() {
    std::vector<PendingRenderer> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_renderer_mu);
        snapshot = g_renderer_pending;
    }
    if (snapshot.empty()) return;

    void* iso = g_r.Isolate_TryGetCurrent();
    if (!iso) return;
    alignas(16) uint8_t hs_buf[kHandleScopeBytes];
    std::memset(hs_buf, 0, sizeof(hs_buf));
    g_r.HandleScope_ctor(hs_buf, iso);
    void* ctx = nullptr;
    g_r.Isolate_GetCurrentContext(iso, &ctx);
    if (!ctx) { g_r.HandleScope_dtor(hs_buf); return; }

    // Build "[id1,id2,...]" id list once, query all keys in one round-trip.
    std::string ids_arr = "[";
    for (size_t i = 0; i < snapshot.size(); ++i) {
        if (i) ids_arr.push_back(',');
        ids_arr += std::to_string(snapshot[i].id);
    }
    ids_arr += "]";
    std::string js =
        "(function(ids){var out={};for(var i=0;i<ids.length;i++){"
        "var k='_pR'+ids[i];var v=globalThis[k];"
        "if(typeof v==='string'){out[ids[i]]=v;delete globalThis[k];}}"
        "return JSON.stringify(out);})(" + ids_arr + ")";

    // Run any pending microtasks first — that's where webContents
    // .executeJavaScript promises resolve and write to globalThis._pR<id>.
    if (g_r.Isolate_PerformMicrotaskCheckpoint) {
        g_r.Isolate_PerformMicrotaskCheckpoint(iso);
    }

    std::string raw, err;
    bool ok = run_script_get_string(iso, ctx, js, raw, err);
    g_r.HandleScope_dtor(hs_buf);
    if (!ok) {
        positron::log::warn("renderer poll script failed: " + err);
        return;
    }

    nlohmann::json results;
    try { results = nlohmann::json::parse(raw); }
    catch (...) { positron::log::warn("renderer poll bad JSON"); return; }

    auto now = std::chrono::steady_clock::now();
    std::vector<PendingRenderer> survivors;
    survivors.reserve(snapshot.size());

    for (auto& p : snapshot) {
        std::string key = std::to_string(p.id);
        if (results.contains(key)) {
            // Resolved.
            positron::v8b::V8Bridge::EvalOutcome o{};
            apply_wrapper_json(results[key].get<std::string>(), o);
            try { p.done(std::move(o)); } catch (...) {}
        } else if (now > p.deadline) {
            positron::v8b::V8Bridge::EvalOutcome o{};
            o.ok = false;
            o.error_message = "renderer eval timed out";
            try { p.done(std::move(o)); } catch (...) {}
        } else {
            survivors.push_back(std::move(p));
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_renderer_mu);
        // Drop the IDs we just resolved — only keep entries still pending
        // (others may have been added between the snapshot and now; we leave
        // those untouched).
        std::vector<PendingRenderer> kept;
        kept.reserve(g_renderer_pending.size());
        for (auto& p : g_renderer_pending) {
            // Was this id in the snapshot AND still pending? If yes, retain.
            bool snap = false, survive = false;
            for (auto& s : snapshot) if (s.id == p.id) { snap = true; break; }
            if (snap) {
                for (auto& s : survivors) if (s.id == p.id) { survive = true; break; }
                if (survive) kept.push_back(std::move(p));
                // else: resolved/timed out — drop.
            } else {
                // Added after we snapshotted; keep.
                kept.push_back(std::move(p));
            }
        }
        g_renderer_pending = std::move(kept);
    }
}

void ensure_poller_running() {
    bool was = g_poller_started.exchange(true);
    if (was) return;
    std::thread([]{
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            {
                std::lock_guard<std::mutex> lk(g_renderer_mu);
                if (g_renderer_pending.empty()) continue;
            }
            positron::v8b::V8Bridge::instance().run_on_v8_thread(
                &poll_renderer_pending_on_v8_thread);
        }
    }).detach();
}

} // anonymous

namespace positron::v8b {

bool V8Bridge::eval_async(std::string code, std::function<void(EvalOutcome)> done) {
    if (!g_ready.load(std::memory_order_acquire)) return false;

    using nlohmann::json;
    std::string js_string_literal = json(code).dump();
    std::string wrapped =
        std::string{"(function(__src){try{var v=(0,eval)(__src);return JSON.stringify({type:typeof v,value:v})}catch(e){return JSON.stringify({error:{message:String((e&&e.message)||e),stack:String((e&&e.stack)||\"\")}})}})("} +
        js_string_literal + ")";

    return run_on_v8_thread([wrapped = std::move(wrapped), done = std::move(done)]() mutable {
        EvalOutcome out{};
        void* iso = g_r.Isolate_TryGetCurrent();
        if (!iso) { out.error_message = "no isolate on this thread"; done(std::move(out)); return; }

        alignas(16) uint8_t hs_buf[kHandleScopeBytes];
        std::memset(hs_buf, 0, sizeof(hs_buf));
        g_r.HandleScope_ctor(hs_buf, iso);
        void* ctx = nullptr;
        g_r.Isolate_GetCurrentContext(iso, &ctx);
        if (!ctx) {
            g_r.HandleScope_dtor(hs_buf);
            out.error_message = "no current context"; done(std::move(out)); return;
        }

        std::string raw, err;
        bool ok = run_script_get_string(iso, ctx, wrapped, raw, err);
        g_r.HandleScope_dtor(hs_buf);
        if (!ok) { out.error_message = err; done(std::move(out)); return; }

        apply_wrapper_json(raw, out);
        done(std::move(out));
    });
}

bool V8Bridge::eval_renderer_async(std::string code, int window_index,
                                    std::function<void(EvalOutcome)> done) {
    if (!g_ready.load(std::memory_order_acquire)) return false;

    uint64_t id = g_renderer_id_counter.fetch_add(1, std::memory_order_relaxed);
    using nlohmann::json;
    std::string user_src_lit = json(code).dump();

    // Launcher: schedule the renderer call; the .then handlers park the
    // settled result onto globalThis under a unique key, where the poller
    // picks it up.
    std::string key = "_pR" + std::to_string(id);
    std::string idx = std::to_string(window_index);
    // The launcher must:
    //   1. resolve the electron module robustly — some Electron embeddings
    //      (Bluebook, packaged apps with security hardening) delete the
    //      global `require`, so we fall back through process.mainModule.require
    //      and finally process._linkedBinding('electron_browser_window');
    //   2. dispatch webContents.executeJavaScript(..., true);
    //   3. park the settled result on globalThis._pR<id>;
    //   4. return a v8::String — trailing `;""` keeps WriteUtf8V2 happy
    //      regardless of what the IIFE itself produces.
    std::string launcher =
        std::string{"(function(){"} +
        "var K='" + key + "';"
        "function _RE(){"
            "if(typeof require==='function'){try{return require('electron');}catch(e){}}"
            "try{if(process&&process.mainModule&&process.mainModule.require)"
                "return process.mainModule.require('electron');}catch(e){}"
            "try{if(process&&typeof process._linkedBinding==='function'){"
                "var w=process._linkedBinding('electron_browser_window');"
                "if(w&&w.BrowserWindow)return{BrowserWindow:w.BrowserWindow};"
            "}}catch(e){}"
            "throw new Error('cannot resolve electron module (no require, no process.mainModule.require, no _linkedBinding)');"
        "}"
        "try{"
            "var BW=_RE().BrowserWindow;"
            "if(!BW||typeof BW.getAllWindows!=='function')throw new Error('BrowserWindow.getAllWindows not available');"
            "var ws=BW.getAllWindows();"
            "var w=ws[" + idx + "];"
            "if(!w){globalThis[K]=JSON.stringify({error:{message:'no BrowserWindow at index " + idx + " (have '+ws.length+')',stack:''}});return;}"
            "w.webContents.executeJavaScript(" + user_src_lit + ",true).then("
                "function(v){try{globalThis[K]=JSON.stringify({type:typeof v,value:v});}"
                "catch(e){globalThis[K]=JSON.stringify({type:typeof v,value:String(v)});}},"
                "function(e){globalThis[K]=JSON.stringify({error:{message:String((e&&e.message)||e),stack:String((e&&e.stack)||\"\")}});}"
            ");"
        "}catch(e){globalThis[K]=JSON.stringify({error:{message:String((e&&e.message)||e),stack:String((e&&e.stack)||\"\")}});}"
        "})();''";

    PendingRenderer pe;
    pe.id = id;
    pe.done = std::move(done);
    pe.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

    {
        std::lock_guard<std::mutex> lk(g_renderer_mu);
        g_renderer_pending.push_back(std::move(pe));
    }
    ensure_poller_running();

    return run_on_v8_thread([launcher = std::move(launcher), id]() mutable {
        void* iso = g_r.Isolate_TryGetCurrent();
        if (!iso) return;
        alignas(16) uint8_t hs_buf[kHandleScopeBytes];
        std::memset(hs_buf, 0, sizeof(hs_buf));
        g_r.HandleScope_ctor(hs_buf, iso);
        void* ctx = nullptr;
        g_r.Isolate_GetCurrentContext(iso, &ctx);
        if (!ctx) {
            g_r.HandleScope_dtor(hs_buf);
            std::lock_guard<std::mutex> lk(g_renderer_mu);
            for (auto it = g_renderer_pending.begin(); it != g_renderer_pending.end(); ++it) {
                if (it->id == id) {
                    EvalOutcome o{}; o.error_message = "no current context for launcher";
                    try { it->done(std::move(o)); } catch (...) {}
                    g_renderer_pending.erase(it);
                    return;
                }
            }
            return;
        }
        std::string raw, err;
        run_script_get_string(iso, ctx, launcher, raw, err);
        // Drain any microtasks scheduled by the launcher (e.g. .then() chains
        // on the executeJavaScript promise) — Electron's microtask policy may
        // be explicit, so we can't rely on Compile/Run auto-running them.
        if (g_r.Isolate_PerformMicrotaskCheckpoint) {
            g_r.Isolate_PerformMicrotaskCheckpoint(iso);
        }
        g_r.HandleScope_dtor(hs_buf);
        if (!err.empty()) positron::log::warn("renderer launcher failed: " + err);
    });
}

} // namespace positron::v8b
