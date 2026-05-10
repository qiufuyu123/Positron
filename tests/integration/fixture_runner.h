#pragma once
#include <cstdint>
#include <string>

namespace positron::test {

struct Fixture {
    uint32_t main_pid;
    uint32_t renderer_pid;
    void* primary_thread; // HANDLE; non-null only when spawned suspended
};

// Spawn the fixture electron process. If `suspended` is true the primary
// thread is created suspended; the caller must call resume_main(fx) once
// the payload has been injected so main.js (and thus require(addon)) runs
// only after the payload's hooks are in place.
Fixture start_fixture(bool suspended = false);
void resume_main(Fixture& fx);
void wait_for_renderer(Fixture& fx, uint32_t timeout_ms = 30000);
void kill_fixture(const Fixture&);
std::wstring abs_payload_dll();

} // namespace positron::test
