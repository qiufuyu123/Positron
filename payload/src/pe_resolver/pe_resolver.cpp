#include "pe_resolver.h"
#include <Windows.h>
#include <Psapi.h>
#include <stdexcept>

namespace positron::pe {

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

static std::optional<ResolveResult> try_module(HMODULE m, const std::string& marker) {
    if (!m) return std::nullopt;
    try {
        auto t = parse_exports(m);
        if (t.rva_by_name.count(marker)) {
            wchar_t name[MAX_PATH] = {};
            ::GetModuleFileNameW(m, name, MAX_PATH);
            return ResolveResult{ std::move(t), std::wstring{name} };
        }
    } catch (...) { /* skip */ }
    return std::nullopt;
}

std::optional<ResolveResult> find_exports_with(const std::string& marker) {
    // Fast path: small set of well-known candidates.
    HMODULE candidates[8]{};
    int n = 0;
    candidates[n++] = ::GetModuleHandleW(nullptr);
    candidates[n++] = ::GetModuleHandleW(L"node.dll");
    candidates[n++] = ::GetModuleHandleW(L"libnode.dll");
    candidates[n++] = ::GetModuleHandleW(L"electron.exe");

    for (int i = 0; i < n; ++i) {
        if (auto r = try_module(candidates[i], marker)) return r;
    }

    // Fallback: scan every loaded module. Useful for symbols that live in a
    // loaded native addon (e.g. addon-exported `napi_register_module_v1`) or
    // a renderer-side helper DLL.
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
        if (auto r = try_module(m, marker)) return r;
    }
    return std::nullopt;
}

} // namespace positron::pe
