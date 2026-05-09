#pragma once
#include <cstdint>
#include <string>
#include <variant>

namespace positron::injector {

struct Success { uint64_t module_base; };
struct Error   { std::string message; };
using Result = std::variant<Success, Error>;

struct Options {
    uint32_t pid;
    std::wstring dll_absolute_path;
    uint32_t timeout_ms = 10000;
};

Result inject(const Options&);

bool target_is_x64(uint32_t pid, std::string& err);

} // namespace positron::injector
