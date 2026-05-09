#pragma once
#include <cstdint>
#include <string>

namespace positron::hook {

struct Handle { uint64_t id; };

void initialize();
void shutdown();

int install(void* target, void* detour, void** original_out, Handle* out);
int uninstall(Handle);

} // namespace positron::hook
