#include "pch.h"
#include "hook_engine.h"
#include "ring_buffer.h"
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

// =============================================================================
// User-level hook pool: kMaxUserSlots fixed __fastcall detours, each capturing
// the first four register-passed args (Win x64 ABI: RCX, RDX, R8, R9) and
// pushing a UserHit record into a SPSC ring buffer. The V8 thread drains the
// ring inside its uv_async_t callback and pushes hook.hit IPC events.

static RingBuffer<64 * 1024> g_hit_ring;
static std::atomic<uint32_t>  g_dropped_since_last{0};

struct Slot {
    uint64_t hook_id   = 0;
    void*    original  = nullptr;   // trampoline, called by detour
    void*    target    = nullptr;   // stored so we can DisableHook on shutdown
};
static Slot          g_slots[kMaxUserSlots]{};
static std::mutex    g_slot_install_mu;

template <int Idx>
static uint64_t __fastcall detour_slot(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    UserHit hit{ g_slots[Idx].hook_id, { a, b, c, d } };
    if (!g_hit_ring.push(&hit, sizeof(hit))) {
        g_dropped_since_last.fetch_add(1, std::memory_order_relaxed);
    }
    using OrigFn = uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t);
    return reinterpret_cast<OrigFn>(g_slots[Idx].original)(a, b, c, d);
}

// Index → detour-function pointer. Generated via template so each slot has a
// distinct entry point that captures its own slot index in code.
static void* slot_detour(int idx) {
    switch (idx) {
        case 0: return reinterpret_cast<void*>(&detour_slot<0>);
        case 1: return reinterpret_cast<void*>(&detour_slot<1>);
        case 2: return reinterpret_cast<void*>(&detour_slot<2>);
        case 3: return reinterpret_cast<void*>(&detour_slot<3>);
        case 4: return reinterpret_cast<void*>(&detour_slot<4>);
        case 5: return reinterpret_cast<void*>(&detour_slot<5>);
        case 6: return reinterpret_cast<void*>(&detour_slot<6>);
        case 7: return reinterpret_cast<void*>(&detour_slot<7>);
        default: return nullptr;
    }
}

int install_user_hook(uint64_t hook_id, void* target) {
    std::lock_guard<std::mutex> lk(g_slot_install_mu);
    for (int i = 0; i < kMaxUserSlots; ++i) {
        if (g_slots[i].hook_id == 0) {
            void* original = nullptr;
            if (auto s = MH_CreateHook(target, slot_detour(i), &original); s != MH_OK)
                return -static_cast<int>(s);
            if (auto s = MH_EnableHook(target); s != MH_OK) {
                MH_RemoveHook(target);
                return -static_cast<int>(s);
            }
            g_slots[i].hook_id  = hook_id;
            g_slots[i].original = original;
            g_slots[i].target   = target;
            return i;
        }
    }
    return -1;
}

bool drain_user_hit(UserHit* out, uint32_t* dropped_since_last) {
    if (!out) return false;
    auto n = g_hit_ring.pop(out, sizeof(*out));
    if (n != sizeof(*out)) return false;
    if (dropped_since_last) {
        *dropped_since_last = g_dropped_since_last.exchange(0, std::memory_order_relaxed);
    }
    return true;
}

} // namespace positron::hook
