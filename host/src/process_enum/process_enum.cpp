#include "process_enum.h"
#include <Windows.h>
#include <winternl.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <algorithm>

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

    PVOID upp = nullptr;
    SIZE_T r;
    if (!::ReadProcessMemory(h, (BYTE*)pbi.PebBaseAddress + 0x20, &upp, sizeof(upp), &r) || r != sizeof(upp) || !upp) return {};

    UNICODE_STRING cl{};
    if (!::ReadProcessMemory(h, (BYTE*)upp + 0x70, &cl, sizeof(cl), &r) || r != sizeof(cl) || !cl.Buffer) return {};

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

static bool exe_looks_like_electron(const wchar_t* path) {
    if (!path) return false;
    std::wstring p = path;
    std::transform(p.begin(), p.end(), p.begin(), ::towlower);
    if (p.find(L"electron") != std::wstring::npos) return true;
    return false;
}

std::vector<Entry> enumerate_electron() {
    std::vector<Entry> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (!::Process32FirstW(snap, &pe)) { ::CloseHandle(snap); return out; }
    do {
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
        wchar_t path[MAX_PATH] = {};
        DWORD pathLen = MAX_PATH;
        if (h) ::QueryFullProcessImageNameW(h, 0, path, &pathLen);

        if (!exe_looks_like_electron(path)) {
            if (h) ::CloseHandle(h);
            continue;
        }
        Entry e;
        e.pid = pe.th32ProcessID;
        e.parent_pid = pe.th32ParentProcessID;
        e.exe_name = pe.szExeFile;
        e.full_path = path;
        if (h) {
            e.command_line = read_command_line(h);
            ::CloseHandle(h);
        }
        e.type = classify(e.command_line);
        out.push_back(std::move(e));
    } while (::Process32NextW(snap, &pe));
    ::CloseHandle(snap);
    return out;
}

} // namespace positron::proc
