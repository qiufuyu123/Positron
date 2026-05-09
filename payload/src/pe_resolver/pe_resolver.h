#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>

namespace positron::pe {

struct ExportTable {
    std::unordered_map<std::string, uintptr_t> rva_by_name;
    uintptr_t image_base = 0;

    std::optional<uintptr_t> abs(const std::string& name) const {
        auto it = rva_by_name.find(name);
        if (it == rva_by_name.end()) return std::nullopt;
        return image_base + it->second;
    }
};

ExportTable parse_exports(const void* module_base);

struct ResolveResult {
    ExportTable table;
    std::wstring module_name;
};
std::optional<ResolveResult> find_exports_with(const std::string& marker_symbol);

} // namespace positron::pe
