#pragma once
#include <cstdint>

namespace positron::teardown {

// Tear down the manually-mapped payload by:
//   1. Setting in-memory shutdown flags so the renderer-poller and
//      hit-flusher detached threads exit on their next tick.
//   2. Asking the v1 ipc_server to drop its socket (request_stop, no join).
//   3. Allocating a scratch RWX page outside our .text, writing a tiny
//      trampoline + a TeardownData struct into it, and spawning a thread
//      whose entry point IS that trampoline. The trampoline:
//         - Sleep(delay_ms)         -- give all our threads time to exit
//         - VirtualFree(base, 0, MEM_RELEASE)  -- free the entire payload
//                                                 image we were mapped at
//         - ExitThread(0)
//
// The trampoline runs entirely in the scratch page + kernel32.dll, never
// touching payload .text after it sleeps. So once the freed pages take
// effect, no live thread has any frame in our code.
//
// Returns true if scheduled (scratch alloc + thread create succeeded).
// On failure the payload stays mapped (caller can keep operating).
bool schedule_unmap(uint32_t delay_ms = 500);

} // namespace positron::teardown
