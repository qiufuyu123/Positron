#include "pe_resolver.h"
#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <stdexcept>

namespace positron::pe {

static bool name_matches(const std::string& name, const SymbolPattern& p);

ExportTable parse_exports(const void* base) {
    auto* dos = static_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) throw std::runtime_error("not MZ");
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        static_cast<const uint8_t*>(base) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) throw std::runtime_error("not PE");

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.Size == 0 || dir.VirtualAddress == 0) {
        return ExportTable{ {}, reinterpret_cast<uintptr_t>(base) };
    }

    auto rva_to_ptr = [&](DWORD rva) -> const uint8_t* {
        return static_cast<const uint8_t*>(base) + rva;
    };
    auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(rva_to_ptr(dir.VirtualAddress));
    auto* names    = reinterpret_cast<const DWORD*>(rva_to_ptr(exp->AddressOfNames));
    auto* funcs    = reinterpret_cast<const DWORD*>(rva_to_ptr(exp->AddressOfFunctions));
    auto* ordinals = reinterpret_cast<const WORD*>(rva_to_ptr(exp->AddressOfNameOrdinals));

    ExportTable t;
    t.image_base = reinterpret_cast<uintptr_t>(base);
    t.rva_by_name.reserve(exp->NumberOfNames);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(rva_to_ptr(names[i]));
        DWORD rva = funcs[ordinals[i]];
        t.rva_by_name.emplace(name, static_cast<uintptr_t>(rva));
    }
    return t;
}

// Returns ResolveResult if module `m` parses successfully and `predicate`
// returns true on its export table.
template <typename Pred>
static std::optional<ResolveResult> try_module_pred(HMODULE m, Pred&& pred) {
    if (!m) return std::nullopt;
    try {
        auto t = parse_exports(m);
        if (pred(t)) {
            wchar_t name[MAX_PATH] = {};
            ::GetModuleFileNameW(m, name, MAX_PATH);
            return ResolveResult{ std::move(t), std::wstring{name} };
        }
    } catch (...) { /* skip */ }
    return std::nullopt;
}

template <typename Pred>
static std::optional<ResolveResult> find_exports_pred(Pred&& pred) {
    HMODULE candidates[8]{};
    int n = 0;
    candidates[n++] = ::GetModuleHandleW(nullptr);
    candidates[n++] = ::GetModuleHandleW(L"node.dll");
    candidates[n++] = ::GetModuleHandleW(L"libnode.dll");
    candidates[n++] = ::GetModuleHandleW(L"electron.exe");

    for (int i = 0; i < n; ++i) {
        if (auto r = try_module_pred(candidates[i], pred)) return r;
    }

    HANDLE proc = ::GetCurrentProcess();
    DWORD needed = 0;
    if (!::EnumProcessModules(proc, nullptr, 0, &needed) || needed == 0) {
        return std::nullopt;
    }
    std::vector<HMODULE> mods(needed / sizeof(HMODULE));
    if (!::EnumProcessModules(proc, mods.data(),
                              static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed)) {
        return std::nullopt;
    }
    mods.resize(needed / sizeof(HMODULE));
    for (HMODULE m : mods) {
        if (auto r = try_module_pred(m, pred)) return r;
    }
    return std::nullopt;
}

std::optional<ResolveResult> find_exports_with(const std::string& marker) {
    return find_exports_pred([&](const ExportTable& t){
        return t.rva_by_name.count(marker) != 0;
    });
}

std::optional<ResolveResult> find_exports_matching(const SymbolPattern& p) {
    return find_exports_pred([&](const ExportTable& t){
        for (auto& kv : t.rva_by_name) {
            if (name_matches(kv.first, p)) return true;
        }
        return false;
    });
}

static bool name_matches(const std::string& name, const SymbolPattern& p) {
    for (auto sv : p.must_contain) {
        if (name.find(sv) == std::string::npos) return false;
    }
    for (auto sv : p.must_not_contain) {
        if (name.find(sv) != std::string::npos) return false;
    }
    return true;
}

std::vector<ResolvedSymbol> find_all(const ExportTable& t, const SymbolPattern& p) {
    std::vector<ResolvedSymbol> out;
    for (auto& kv : t.rva_by_name) {
        if (name_matches(kv.first, p)) {
            out.push_back({ kv.first, t.image_base + kv.second });
        }
    }
    return out;
}

std::optional<ResolvedSymbol> find_one(const ExportTable& t, const SymbolPattern& p) {
    auto matches = find_all(t, p);
    if (matches.empty()) return std::nullopt;
    if (matches.size() == 1) return matches[0];
    // Multiple matches: pick the shortest mangled name as a heuristic for the
    // least-specialised overload. (More template parameters / extra args
    // typically grow the mangled length.)
    std::sort(matches.begin(), matches.end(), [](auto& a, auto& b){
        return a.name.size() < b.name.size();
    });
    return matches.front();
}

} // namespace positron::pe
