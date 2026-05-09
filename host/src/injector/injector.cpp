#include "injector.h"
#include <Windows.h>
#include <Psapi.h>

namespace positron::injector {

bool target_is_x64(uint32_t pid, std::string& err) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { err = "OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) failed: " + std::to_string(::GetLastError()); return false; }
    USHORT proc = 0, mach = 0;
    BOOL ok = ::IsWow64Process2(h, &proc, &mach);
    ::CloseHandle(h);
    if (!ok) { err = "IsWow64Process2 failed: " + std::to_string(::GetLastError()); return false; }
    USHORT effective = (proc == IMAGE_FILE_MACHINE_UNKNOWN) ? mach : proc;
    if (effective == IMAGE_FILE_MACHINE_AMD64) return true;
    err = "target architecture " + std::to_string(effective) + " is not x64";
    return false;
}

Result inject(const Options& opt) {
    std::string err;
    if (!target_is_x64(opt.pid, err)) return Error{err};

    HANDLE proc = ::OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, opt.pid);
    if (!proc) return Error{"OpenProcess: " + std::to_string(::GetLastError())};

    SIZE_T bytes = (opt.dll_absolute_path.size() + 1) * sizeof(wchar_t);
    LPVOID remote = ::VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { ::CloseHandle(proc); return Error{"VirtualAllocEx failed"}; }

    SIZE_T written = 0;
    if (!::WriteProcessMemory(proc, remote, opt.dll_absolute_path.c_str(), bytes, &written) || written != bytes) {
        ::VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        ::CloseHandle(proc);
        return Error{"WriteProcessMemory failed"};
    }

    auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    HANDLE thr = ::CreateRemoteThread(proc, nullptr, 0, loadLibraryW, remote, 0, nullptr);
    if (!thr) {
        ::VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        ::CloseHandle(proc);
        return Error{"CreateRemoteThread failed: " + std::to_string(::GetLastError())};
    }

    DWORD wait = ::WaitForSingleObject(thr, opt.timeout_ms);
    DWORD exitCode = 0;
    if (wait == WAIT_OBJECT_0) ::GetExitCodeThread(thr, &exitCode);

    ::CloseHandle(thr);
    ::VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    ::CloseHandle(proc);

    if (wait != WAIT_OBJECT_0) return Error{"timeout waiting for LoadLibraryW remote thread"};
    if (exitCode == 0) return Error{"LoadLibraryW returned NULL in target (DLL failed to load)"};

    return Success{ exitCode };
}

} // namespace positron::injector
