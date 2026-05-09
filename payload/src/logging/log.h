#pragma once
#include <cstdint>
#include <string>

namespace positron::log {

void init(uint32_t pid);
void info(std::string s);
void warn(std::string s);
void error(std::string s);

using ForwardFn = void(*)(const std::string& level, const std::string& msg);
void set_forwarder(ForwardFn f);

} // namespace positron::log
