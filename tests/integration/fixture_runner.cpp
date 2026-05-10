#include "fixture_runner.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <filesystem>

namespace positron::test {

static std::vector<DWORD> children_of(DWORD parent) {
    std::vector<DWORD> out;
    HANDLE s = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (::Process32FirstW(s, &pe)) {
        do { if (pe.th32ParentProcessID == parent) out.push_back(pe.th32ProcessID); } while (::Process32NextW(s, &pe));
    }
    ::CloseHandle(s);
    return out;
}

static std::wstring read_cmdline(DWORD pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return {};
    using NTSTATUS = LONG;
    typedef NTSTATUS(NTAPI* NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    auto NtQIP = reinterpret_cast<NtQIP_t>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    struct PBI { PVOID r1; PVOID Peb; PVOID r2[2]; ULONG_PTR Pid; PVOID r3; } pbi{};
    ULONG ret = 0;
    if (!NtQIP || NtQIP(h, 0, &pbi, sizeof(pbi), &ret) != 0 || !pbi.Peb) { ::CloseHandle(h); return {}; }
    PVOID upp = nullptr; SIZE_T r;
    if (!::ReadProcessMemory(h, (BYTE*)pbi.Peb + 0x20, &upp, sizeof(upp), &r) || !upp) { ::CloseHandle(h); return {}; }
    UNICODE_STRING cl{};
    if (!::ReadProcessMemory(h, (BYTE*)upp + 0x70, &cl, sizeof(cl), &r) || !cl.Buffer) { ::CloseHandle(h); return {}; }
    std::wstring out(cl.Length / sizeof(wchar_t), L'\0');
    ::ReadProcessMemory(h, cl.Buffer, out.data(), cl.Length, &r);
    ::CloseHandle(h);
    return out;
}

static DWORD find_renderer(DWORD root_pid) {
    std::vector<DWORD> queue{ root_pid };
    std::vector<DWORD> seen{ root_pid };
    for (size_t i = 0; i < queue.size(); ++i) {
        for (auto c : children_of(queue[i])) {
            if (std::find(seen.begin(), seen.end(), c) == seen.end()) {
                queue.push_back(c); seen.push_back(c);
            }
        }
    }
    for (auto pid : seen) {
        auto cl = read_cmdline(pid);
        std::wstring lcl = cl;
        std::transform(lcl.begin(), lcl.end(), lcl.begin(), ::towlower);
        if (lcl.find(L"electron") == std::wstring::npos) continue;
        if (cl.find(L"--type=renderer") != std::wstring::npos) {
            return pid;
        }
    }
    return 0;
}

Fixture start_fixture(bool suspended) {
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    // Use the electron.exe binary directly to avoid npm/cmd intermediaries
    // (fewer ancestor processes to walk).
    std::wstring cmd = L"\"tests\\fixtures\\sample-electron-app\\node_modules\\electron\\dist\\electron.exe\" tests\\fixtures\\sample-electron-app";
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(L'\0');

    DWORD flags = CREATE_NO_WINDOW;
    if (suspended) flags |= CREATE_SUSPENDED;
    if (!::CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE,
                          flags, nullptr, nullptr, &si, &pi))
        throw std::runtime_error("failed to start fixture: " + std::to_string(::GetLastError()));

    Fixture out{};
    out.main_pid = pi.dwProcessId;
    out.renderer_pid = 0;
    out.primary_thread = suspended ? pi.hThread : nullptr;

    if (!suspended) {
        ::CloseHandle(pi.hThread);
        // Wait for the renderer to appear so callers that need it can rely on
        // a fully-running process tree.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD r = find_renderer(pi.dwProcessId);
            if (r) { out.renderer_pid = r; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    ::CloseHandle(pi.hProcess);
    return out;
}

void resume_main(Fixture& fx) {
    if (fx.primary_thread) {
        ::ResumeThread(reinterpret_cast<HANDLE>(fx.primary_thread));
        ::CloseHandle(reinterpret_cast<HANDLE>(fx.primary_thread));
        fx.primary_thread = nullptr;
    }
}

void wait_for_renderer(Fixture& fx, uint32_t timeout_ms) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD r = find_renderer(fx.main_pid);
        if (r) { fx.renderer_pid = r; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void kill_fixture(const Fixture& f) {
    if (f.primary_thread) {
        // The primary thread is still suspended; resume so we can terminate cleanly.
        ::ResumeThread(reinterpret_cast<HANDLE>(f.primary_thread));
        ::CloseHandle(reinterpret_cast<HANDLE>(f.primary_thread));
    }
    auto kill = [](DWORD pid) {
        HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) { ::TerminateProcess(h, 0); ::CloseHandle(h); }
    };
    // Walk the process tree and kill all descendants of main.
    if (f.main_pid) {
        std::vector<DWORD> queue{ f.main_pid };
        std::vector<DWORD> seen{ f.main_pid };
        for (size_t i = 0; i < queue.size(); ++i) {
            for (auto c : children_of(queue[i])) {
                if (std::find(seen.begin(), seen.end(), c) == seen.end()) {
                    queue.push_back(c); seen.push_back(c);
                }
            }
        }
        for (auto pid : seen) kill(pid);
    }
}

std::wstring abs_payload_dll() {
    wchar_t buf[MAX_PATH];
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    auto p = std::filesystem::path(buf).parent_path() / L"payload.dll";
    return p.wstring();
}

} // namespace positron::test
