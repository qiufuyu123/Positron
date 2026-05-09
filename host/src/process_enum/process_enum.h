#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace positron::proc {

enum class ElectronType { Unknown, Main, Renderer, Gpu, Utility };

struct Entry {
    uint32_t pid;
    uint32_t parent_pid;
    std::wstring exe_name;
    std::wstring full_path;
    std::wstring command_line;
    ElectronType type;
};

std::vector<Entry> enumerate_electron();

const wchar_t* type_name(ElectronType);

} // namespace positron::proc
