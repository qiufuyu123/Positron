#include "pch.h"
#include "napi_bridge.h"
#include "hook_engine/hook_engine.h"
#include "logging/log.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
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

// Resolve the napi runtime functions (napi_run_script etc.) from whatever
// module exports them — typically electron.exe / node.dll. This is independent
// of whether a native addon has loaded yet: the runtime symbols always live
// in the host process binary.
static bool g_runtime_resolved = false;
static std::mutex g_install_mu;

static bool resolve_napi_runtime(Bridge& self) {
    auto found = pe::find_exports_with("napi_run_script");
    if (!found) {
        log::warn("napi runtime exports (e.g. napi_run_script) not found in any module");
        return false;
    }
    auto& t = found->table;
    auto resolve = [&](const char* n) -> void* {
        auto a = t.abs(n);
        return a ? reinterpret_cast<void*>(*a) : nullptr;
    };
    self.run_script                   = (napi_run_script_fn)                   resolve("napi_run_script");
    self.create_string_utf8           = (napi_create_string_utf8_fn)           resolve("napi_create_string_utf8");
    self.get_and_clear_last_exception = (napi_get_and_clear_last_exception_fn) resolve("napi_get_and_clear_last_exception");
    self.open_handle_scope            = (napi_open_handle_scope_fn)            resolve("napi_open_handle_scope");
    self.close_handle_scope           = (napi_close_handle_scope_fn)           resolve("napi_close_handle_scope");
    self.get_uv_event_loop            = (napi_get_uv_event_loop_fn)            resolve("napi_get_uv_event_loop");
    self.get_global                   = (napi_get_global_fn)                   resolve("napi_get_global");
    self.get_named_property           = (napi_get_named_property_fn)           resolve("napi_get_named_property");
    self.call_function                = (napi_call_function_fn)                resolve("napi_call_function");
    self.get_value_string_utf8        = (napi_get_value_string_utf8_fn)        resolve("napi_get_value_string_utf8");
    self.typeof_                      = (napi_typeof_fn)                       resolve("napi_typeof");

    g_uv_async_init_resolved = (uv_async_init_fn) resolve("uv_async_init");
    g_uv_async_send          = (uv_async_send_fn) resolve("uv_async_send");

    for (auto& kv : t.rva_by_name) {
        if (kv.first.rfind("napi_", 0) == 0) self.present_symbols.push_back(kv.first);
    }
    self.cached_table = std::move(found->table);
    return true;
}

// Find the napi_register_module_v1 symbol (which is exported by native addons,
// not by the runtime itself) and install the hook that captures napi_env on
// first invocation. Returns true on success. If no addon has been loaded yet
// this returns false; the caller polls until an addon appears.
static bool try_install_register_hook() {
    auto found = pe::find_exports_with("napi_register_module_v1");
    if (!found) return false;
    auto a = found->table.abs("napi_register_module_v1");
    if (!a) return false;
    void* tgt = reinterpret_cast<void*>(*a);
    hook::Handle h{};
    int rc = hook::install(tgt, reinterpret_cast<void*>(&detour_register_module_v1),
                           reinterpret_cast<void**>(&g_orig_register), &h);
    if (rc != 0) {
        log::error("MinHook install on napi_register_module_v1 failed: " + std::to_string(rc));
        return false;
    }
    log::info("hook installed on napi_register_module_v1");
    return true;
}

static std::atomic<bool> g_register_hook_installed{false};
static std::thread g_register_hook_watcher;
static std::atomic<bool> g_watcher_run{false};

// LoadLibrary* hooks: when an addon (.node) is loaded by Node we want to
// install our napi_register_module_v1 detour BEFORE Node calls
// GetProcAddress + invoke. This catches every addon load, so we don't have
// to race the polling watcher against Node's call sequence.
typedef HMODULE (WINAPI *LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
typedef HMODULE (WINAPI *LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
typedef HMODULE (WINAPI *LoadLibraryW_t)(LPCWSTR);
typedef HMODULE (WINAPI *LoadLibraryA_t)(LPCSTR);
static LoadLibraryExW_t g_orig_LoadLibraryExW = nullptr;
static LoadLibraryExA_t g_orig_LoadLibraryExA = nullptr;
static LoadLibraryW_t   g_orig_LoadLibraryW   = nullptr;
static LoadLibraryA_t   g_orig_LoadLibraryA   = nullptr;

static void try_install_after_load(const char* via) {
    std::lock_guard lk(g_install_mu);
    if (g_register_hook_installed.load()) return;
    if (try_install_register_hook()) {
        g_register_hook_installed = true;
        log::info(std::string("napi bridge: register hook installed via ") + via);
    }
}

static HMODULE WINAPI detour_LoadLibraryExW(LPCWSTR file, HANDLE reserved, DWORD flags) {
    HMODULE h = g_orig_LoadLibraryExW(file, reserved, flags);
    if (h) try_install_after_load("LoadLibraryExW");
    return h;
}
static HMODULE WINAPI detour_LoadLibraryExA(LPCSTR file, HANDLE reserved, DWORD flags) {
    HMODULE h = g_orig_LoadLibraryExA(file, reserved, flags);
    if (h) try_install_after_load("LoadLibraryExA");
    return h;
}
static HMODULE WINAPI detour_LoadLibraryW(LPCWSTR file) {
    HMODULE h = g_orig_LoadLibraryW(file);
    if (h) try_install_after_load("LoadLibraryW");
    return h;
}
static HMODULE WINAPI detour_LoadLibraryA(LPCSTR file) {
    HMODULE h = g_orig_LoadLibraryA(file);
    if (h) try_install_after_load("LoadLibraryA");
    return h;
}

bool Bridge::initialize() {
    std::lock_guard lk(g_install_mu);
    if (!g_runtime_resolved) {
        if (!resolve_napi_runtime(*this)) return false;
        g_runtime_resolved = true;
    }
    if (g_register_hook_installed.load()) return true;

    if (try_install_register_hook()) {
        g_register_hook_installed = true;
        log::info("napi bridge initialized; awaiting first module register");
        return true;
    }

    // No addon loaded yet. Two complementary strategies:
    //
    //   1. Hook LoadLibraryExW so that the moment Node maps the addon DLL
    //      (and BEFORE it calls GetProcAddress + invokes
    //      napi_register_module_v1) we install our detour. This is the
    //      reliable path.
    //   2. Belt-and-braces watcher thread that polls every 50ms in case the
    //      LoadLibrary hook missed it (e.g. an addon was already loaded but
    //      the runtime wasn't resolved yet at that moment).
    log::info("napi_register_module_v1 not present yet; arming LoadLibrary* hooks");
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        auto hook_one = [&](const char* name, void* detour, void** orig) {
            void* tgt = reinterpret_cast<void*>(::GetProcAddress(k32, name));
            if (!tgt) return;
            hook::Handle h{};
            int rc = hook::install(tgt, detour, orig, &h);
            if (rc != 0) {
                log::warn(std::string(name) + " hook install failed: " + std::to_string(rc));
            } else {
                log::info(std::string(name) + " hook installed");
            }
        };
        hook_one("LoadLibraryExW", reinterpret_cast<void*>(&detour_LoadLibraryExW),
                 reinterpret_cast<void**>(&g_orig_LoadLibraryExW));
        hook_one("LoadLibraryExA", reinterpret_cast<void*>(&detour_LoadLibraryExA),
                 reinterpret_cast<void**>(&g_orig_LoadLibraryExA));
        hook_one("LoadLibraryW", reinterpret_cast<void*>(&detour_LoadLibraryW),
                 reinterpret_cast<void**>(&g_orig_LoadLibraryW));
        hook_one("LoadLibraryA", reinterpret_cast<void*>(&detour_LoadLibraryA),
                 reinterpret_cast<void**>(&g_orig_LoadLibraryA));
    }

    if (!g_watcher_run.exchange(true)) {
        g_register_hook_watcher = std::thread([]{
            while (g_watcher_run.load()) {
                {
                    std::lock_guard lk(g_install_mu);
                    if (g_register_hook_installed.load()) return;
                    if (try_install_register_hook()) {
                        g_register_hook_installed = true;
                        log::info("napi bridge: register hook installed by watcher");
                        return;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    return true; // runtime is resolved; hook will be installed later
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
