#pragma once
#include <cstdint>

namespace positron::pipe { struct Client; }

namespace positron::repl {

// Run an interactive REPL bound to an open pipe::Client. Returns when the
// user types .detach / .quit, or when the connection drops. Returns the
// process exit code (0 = normal).
int run(positron::pipe::Client& c, uint32_t target_pid);

} // namespace positron::repl
