#pragma once
#include <cstdint>
#include <string>

namespace positron::hook {

struct Handle { uint64_t id; };

void initialize();
void shutdown();

int install(void* target, void* detour, void** original_out, Handle* out);
int uninstall(Handle);

// User-facing hook: install a generic counting detour at `target`. Each hit
// records the first four argument registers (RCX/RDX/R8/R9 on Win x64) into a
// ring buffer. A pool of fixed slots backs this — request 0..kMaxUserSlots-1
// hooks at once; install_user_hook returns the slot index or -1 on exhaustion.
constexpr int kMaxUserSlots = 8;

struct UserHit {
    uint64_t hook_id;
    uint64_t args[4];
};

int  install_user_hook(uint64_t hook_id, void* target);
// Drain at most one pending hit. Returns true if `out` was filled.
bool drain_user_hit(UserHit* out, uint32_t* dropped_since_last);

} // namespace positron::hook
