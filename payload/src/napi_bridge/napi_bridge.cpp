#include "pch.h"
#include "napi_bridge.h"
#include "hook_engine/hook_engine.h"
#include "logging/log.h"
#include <Windows.h>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

extern "C" {
typedef struct uv_async_s uv_async_t;
typedef void(*uv_async_cb)(uv_async_t* handle);
typedef int (__cdecl *uv_async_init_fn)(::uv_loop_t* loop, uv_async_t* async, uv_async_cb cb);
typedef int (__cdecl *uv_async_send_fn)(uv_async_t* async);
}

static constexpr size_t kUvAsyncBytes = 1024;

namespace positron::napi {

static std::atomic<napi_env> g_env{nullptr};
static uv_loop_t* g_loop = nullptr;
static uint8_t g_async_storage[kUvAsyncBytes];
static uv_async_t* g_async = reinterpret_cast<uv_async_t*>(g_async_storage);
static std::mutex g_q_mu;
static std::queue<std::function<void(napi_env)>> g_q;

static uv_async_send_fn g_uv_async_send = nullptr;
static uv_async_init_fn g_uv_async_init_resolved = nullptr;

static void async_cb(uv_async_t*) {
    auto env = g_env.load();
    if (!env) return;
    std::queue<std::function<void(napi_env)>> drain;
    {
        std::lock_guard lk(g_q_mu);
        std::swap(drain, g_q);
    }
    while (!drain.empty()) {
        try { drain.front()(env); } catch (...) {}
        drain.pop();
    }
}

static napi_register_module_v1_fn g_orig_register = nullptr;

static napi_value __cdecl detour_register_module_v1(napi_env env, napi_value exports) {
    napi_env expected = nullptr;
    if (g_env.compare_exchange_strong(expected, env)) {
        log::info("napi_env captured");
        if (Bridge::instance().get_uv_event_loop) {
            uv_loop_t* loop = nullptr;
            if (Bridge::instance().get_uv_event_loop(env, &loop) == napi_ok) {
                g_loop = loop;
                if (g_uv_async_init_resolved && g_uv_async_send) {
                    g_uv_async_init_resolved(loop, g_async, &async_cb);
                }
            }
        }
    }
    return g_orig_register(env, exports);
}

Bridge& Bridge::instance() { static Bridge b; return b; }

bool Bridge::has_env() const { return g_env.load() != nullptr; }
napi_env Bridge::env_unsafe() const { return g_env.load(); }

void Bridge::run_on_v8_thread(std::function<void(napi_env)> fn) {
    {
        std::lock_guard lk(g_q_mu);
        g_q.push(std::move(fn));
    }
    if (g_uv_async_send && g_async) g_uv_async_send(g_async);
}

bool Bridge::initialize() {
    auto found = pe::find_exports_with("napi_register_module_v1");
    if (!found) {
        log::warn("napi_register_module_v1 not found in any candidate module");
        return false;
    }
    auto& t = found->table;
    auto resolve = [&](const char* n) -> void* {
        auto a = t.abs(n);
        return a ? reinterpret_cast<void*>(*a) : nullptr;
    };
    run_script                  = (napi_run_script_fn)                  resolve("napi_run_script");
    create_string_utf8          = (napi_create_string_utf8_fn)          resolve("napi_create_string_utf8");
    get_and_clear_last_exception= (napi_get_and_clear_last_exception_fn)resolve("napi_get_and_clear_last_exception");
    open_handle_scope           = (napi_open_handle_scope_fn)           resolve("napi_open_handle_scope");
    close_handle_scope          = (napi_close_handle_scope_fn)          resolve("napi_close_handle_scope");
    get_uv_event_loop           = (napi_get_uv_event_loop_fn)           resolve("napi_get_uv_event_loop");
    get_global                  = (napi_get_global_fn)                  resolve("napi_get_global");
    get_named_property          = (napi_get_named_property_fn)          resolve("napi_get_named_property");
    call_function               = (napi_call_function_fn)               resolve("napi_call_function");
    get_value_string_utf8       = (napi_get_value_string_utf8_fn)       resolve("napi_get_value_string_utf8");
    typeof_                     = (napi_typeof_fn)                      resolve("napi_typeof");

    g_uv_async_init_resolved = (uv_async_init_fn) resolve("uv_async_init");
    g_uv_async_send          = (uv_async_send_fn) resolve("uv_async_send");

    for (auto& kv : t.rva_by_name) {
        if (kv.first.rfind("napi_", 0) == 0) present_symbols.push_back(kv.first);
    }

    void* tgt = reinterpret_cast<void*>(*t.abs("napi_register_module_v1"));
    hook::Handle h{};
    int rc = hook::install(tgt, reinterpret_cast<void*>(&detour_register_module_v1),
                           reinterpret_cast<void**>(&g_orig_register), &h);
    if (rc != 0) {
        log::error("MinHook install on napi_register_module_v1 failed: " + std::to_string(rc));
        return false;
    }
    cached_table = std::move(found->table);
    log::info("napi bridge initialized; awaiting first module register");
    return true;
}

bool Bridge::is_renderer() const {
    static const bool v = []{
        std::wstring cl = ::GetCommandLineW();
        return cl.find(L"--type=renderer") != std::wstring::npos;
    }();
    return v;
}

std::optional<uintptr_t> Bridge::symbol_addr(const std::string& name) const {
    if (cached_table.image_base == 0) return std::nullopt;
    auto it = cached_table.rva_by_name.find(name);
    if (it == cached_table.rva_by_name.end()) return std::nullopt;
    return cached_table.image_base + it->second;
}

void Bridge::stop_kicker() {
    // Implemented in Task 19 once g_kicker_run/g_kicker exist.
}

} // namespace positron::napi
