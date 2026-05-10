#pragma once
#include <cstdint>
#include <string>

namespace positron::log {

void init(uint32_t pid);
void info(std::string s);
void warn(std::string s);
void error(std::string s);

// "Milestone": create a named kernel event Local\positron-mile-<pid>-<n>.
// Even when the payload runs in a sandboxed (Low IL) renderer where file IO
// to %TEMP% / Public is blocked, the host can OpenEventW on the same name
// to confirm a stage was reached.
void milestone(int n);

using ForwardFn = void(*)(const std::string& level, const std::string& msg);
void set_forwarder(ForwardFn f);

} // namespace positron::log
