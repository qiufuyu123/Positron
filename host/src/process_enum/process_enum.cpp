#include "process_enum.h"
#include <Windows.h>
#include <winternl.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "Psapi.lib")

namespace positron::proc {

static std::wstring read_command_line(HANDLE h) {
    using NTSTATUS = LONG;
    typedef NTSTATUS(NTAPI* NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
    auto NtQIP = reinterpret_cast<NtQIP_t>(::GetProcAddress(nt, "NtQueryInformationProcess"));
    if (!NtQIP) return {};

    struct PBI { PVOID Reserved1; PVOID PebBaseAddress; PVOID Reserved2[2]; ULONG_PTR UniqueProcessId; PVOID Reserved3; };
    PBI pbi{};
    ULONG ret = 0;
    if (NtQIP(h, 0, &pbi, sizeof(pbi), &ret) != 0 || !pbi.PebBaseAddress) return {};

    // PEB layout differs between x86 and x64. Use offsets matching this host's
    // architecture — we only ever read same-bitness targets (cross-bitness
    // injection isn't supported by Blackbone's manual mapping anyway).
#ifdef _M_X64
    constexpr SIZE_T kPebProcessParametersOffset = 0x20;
    constexpr SIZE_T kRupCommandLineOffset       = 0x70;
#else
    constexpr SIZE_T kPebProcessParametersOffset = 0x10;
    constexpr SIZE_T kRupCommandLineOffset       = 0x40;
#endif

    PVOID upp = nullptr;
    SIZE_T r;
    if (!::ReadProcessMemory(h, (BYTE*)pbi.PebBaseAddress + kPebProcessParametersOffset,
                             &upp, sizeof(upp), &r) || r != sizeof(upp) || !upp) return {};

    UNICODE_STRING cl{};
    if (!::ReadProcessMemory(h, (BYTE*)upp + kRupCommandLineOffset,
                             &cl, sizeof(cl), &r) || r != sizeof(cl) || !cl.Buffer) return {};

    std::wstring out(cl.Length / sizeof(wchar_t), L'\0');
    if (!::ReadProcessMemory(h, cl.Buffer, out.data(), cl.Length, &r)) return {};
    return out;
}

static ElectronType classify(const std::wstring& cmdline) {
    if (cmdline.find(L"--type=renderer") != std::wstring::npos) return ElectronType::Renderer;
    if (cmdline.find(L"--type=gpu-process") != std::wstring::npos) return ElectronType::Gpu;
    if (cmdline.find(L"--type=utility") != std::wstring::npos) return ElectronType::Utility;
    if (cmdline.find(L"--type=") != std::wstring::npos) return ElectronType::Unknown;
    return ElectronType::Main;
}

const wchar_t* type_name(ElectronType t) {
    switch (t) {
        case ElectronType::Main: return L"main";
        case ElectronType::Renderer: return L"renderer";
        case ElectronType::Gpu: return L"gpu";
        case ElectronType::Utility: return L"utility";
        default: return L"unknown";
    }
}

// Real-world Electron apps don't have "electron" in their exe name (VS Code is
// Code.exe, Discord is Discord.exe, Cursor is Cursor.exe, ...). The reliable
// signal is Chromium's process-tree fingerprint:
//
//   * Child processes pass `--type=renderer/gpu-process/utility/...`.
//   * The main process is the parent of those children, running the SAME exe
//     (Chromium uses one binary in different roles based on --type=).
//
// So we enumerate everything once, then in two passes:
//   pass 1: any process whose command line contains `--type=` is a Chromium
//           child — include it, and remember its parent PID.
//   pass 2: include each remembered parent IFF its exe path matches the child's
//           (filters out cases like explorer.exe being parent of a launcher).

namespace {

struct RawEntry {
    uint32_t pid;
    uint32_t ppid;
    std::wstring exe_name;     // basename, from Process32
    std::wstring full_path;    // absolute, from QueryFullProcessImageName
    std::wstring command_line; // from PEB; empty if inaccessible
};

std::vector<RawEntry> snapshot_all() {
    std::vector<RawEntry> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (::Process32FirstW(snap, &pe)) {
        do {
            RawEntry e{};
            e.pid = pe.th32ProcessID;
            e.ppid = pe.th32ParentProcessID;
            e.exe_name = pe.szExeFile;
            HANDLE h = ::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                FALSE, pe.th32ProcessID);
            if (h) {
                wchar_t path[MAX_PATH] = {};
                DWORD path_len = MAX_PATH;
                if (::QueryFullProcessImageNameW(h, 0, path, &path_len)) {
                    e.full_path = path;
                }
                e.command_line = read_command_line(h);
                ::CloseHandle(h);
            }
            out.push_back(std::move(e));
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return out;
}

bool same_path_ci(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    if (a.size() != b.size()) return false;
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

} // anonymous

std::vector<Entry> enumerate_electron() {
    auto all = snapshot_all();

    // Index by pid for parent-lookup.
    std::unordered_map<uint32_t, const RawEntry*> by_pid;
    by_pid.reserve(all.size());
    for (auto& e : all) by_pid.emplace(e.pid, &e);

    std::unordered_set<uint32_t> include;

    // Pass 1: collect every process with --type=. These are Chromium children.
    for (auto& e : all) {
        if (e.command_line.find(L"--type=") != std::wstring::npos) {
            include.insert(e.pid);
        }
    }

    // Pass 2: for each child, also include its parent IF the parent's exe
    // matches (Chromium reuses one binary across roles).
    std::unordered_set<uint32_t> parents;
    for (auto& e : all) {
        if (e.command_line.find(L"--type=") == std::wstring::npos) continue;
        auto it = by_pid.find(e.ppid);
        if (it == by_pid.end()) continue;
        if (same_path_ci(it->second->full_path, e.full_path)) {
            parents.insert(e.ppid);
        }
    }
    include.insert(parents.begin(), parents.end());

    std::vector<Entry> out;
    out.reserve(include.size());
    for (auto& e : all) {
        if (!include.count(e.pid)) continue;
        Entry x;
        x.pid = e.pid;
        x.parent_pid = e.ppid;
        x.exe_name = e.exe_name;
        x.full_path = e.full_path;
        x.command_line = e.command_line;
        x.type = classify(e.command_line);
        out.push_back(std::move(x));
    }
    return out;
}

} // namespace positron::proc
