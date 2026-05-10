#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

// Same as find_exports_with but the marker is a fuzzy pattern. Used when we
// don't want to hard-code an exact (mangled) symbol name.
std::optional<ResolveResult> find_exports_matching(const struct SymbolPattern&);

// Fuzzy matching against an export table — every name in `must_contain` must
// appear as a substring of the export's name, and no name in `must_not_contain`
// may appear. Used to resolve C++-mangled V8 symbols without hard-coding the
// full decorated name (which drifts across V8 versions, e.g. when extra
// template parameters or overloads are added).
//
// Stored as std::vector so the pattern can be moved/copied across
// translation/storage boundaries — std::initializer_list does not own its
// backing array and is unsafe to keep beyond the enclosing full-expression.
struct SymbolPattern {
    std::vector<std::string_view> must_contain;
    std::vector<std::string_view> must_not_contain;
};

struct ResolvedSymbol {
    std::string name;     // matched mangled/decorated name
    uintptr_t   abs_addr; // image_base + rva
};

// Returns all matches; empty if none.
std::vector<ResolvedSymbol> find_all(const ExportTable&, const SymbolPattern&);

// Returns the unique match if any; if multiple match, picks the one with the
// shortest name (a heuristic for "least-templated overload"). Empty if no match.
std::optional<ResolvedSymbol> find_one(const ExportTable&, const SymbolPattern&);

} // namespace positron::pe
