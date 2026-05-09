#include "pch.h"
#include "hook_engine.h"
#include "logging/log.h"
#include <MinHook.h>
#include <atomic>
#include <unordered_map>
#include <mutex>

namespace positron::hook {

static std::atomic<uint64_t> g_id_counter{1};
static std::mutex g_mu;
static std::unordered_map<uint64_t, void*> g_targets;

void initialize() {
    if (MH_Initialize() != MH_OK) log::error("MH_Initialize failed");
}

void shutdown() {
    std::lock_guard lk(g_mu);
    for (auto& kv : g_targets) MH_DisableHook(kv.second);
    g_targets.clear();
    MH_Uninitialize();
}

int install(void* target, void* detour, void** original_out, Handle* out) {
    if (auto s = MH_CreateHook(target, detour, original_out); s != MH_OK) return static_cast<int>(s);
    if (auto s = MH_EnableHook(target); s != MH_OK) { MH_RemoveHook(target); return static_cast<int>(s); }
    Handle h{ g_id_counter.fetch_add(1) };
    {
        std::lock_guard lk(g_mu);
        g_targets[h.id] = target;
    }
    if (out) *out = h;
    return 0;
}

int uninstall(Handle h) {
    void* t = nullptr;
    {
        std::lock_guard lk(g_mu);
        auto it = g_targets.find(h.id);
        if (it == g_targets.end()) return -1;
        t = it->second;
        g_targets.erase(it);
    }
    MH_DisableHook(t);
    MH_RemoveHook(t);
    return 0;
}

} // namespace positron::hook
