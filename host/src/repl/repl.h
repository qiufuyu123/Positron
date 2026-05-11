#pragma once
#include <cstdint>

namespace positron::sdk { class Session; }

namespace positron::repl {

// Run an interactive REPL bound to an open SDK Session. Returns when the
// user types .detach / .quit, or when the connection drops.
int run(positron::sdk::Session& s, uint32_t target_pid);

} // namespace positron::repl
