#include "injector.h"
#include <Windows.h>

// Blackbone — manual DLL mapping. Bypasses the standard Windows loader so
// the payload can be planted into sandboxed renderer processes that block
// LoadLibrary on unsigned/non-system DLLs.
#include <BlackBone/Process/Process.h>
#include <BlackBone/ManualMap/MMap.h>

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

    const char* arch = "unknown";
    switch (effective) {
        case IMAGE_FILE_MACHINE_I386:  arch = "x86 (32-bit)"; break;
        case IMAGE_FILE_MACHINE_ARM64: arch = "ARM64"; break;
        case IMAGE_FILE_MACHINE_ARM:   arch = "ARM (32-bit)"; break;
        case IMAGE_FILE_MACHINE_ARMNT: arch = "ARM Thumb-2"; break;
        default: break;
    }
    err = std::string{"target is "} + arch + " (machine=0x" +
          [&]{ char b[8]; ::sprintf_s(b, "%X", effective); return std::string{b}; }() +
          "). positron is x64 only — pick an x64 target.";
    return false;
}

Result inject(const Options& opt) {
    std::string err;
    if (!target_is_x64(opt.pid, err)) return Error{err};

    blackbone::Process proc;
    NTSTATUS status = proc.Attach(opt.pid);
    if (!NT_SUCCESS(status)) {
        char buf[64];
        ::sprintf_s(buf, "0x%08lX", status);
        return Error{std::string{"blackbone::Attach failed (NTSTATUS "} + buf + ")"};
    }

    // Mapping flags chosen for compatibility with Chromium-sandboxed renderers:
    //   ManualImports — resolve our payload's IAT without invoking the loader
    //                   (so the loader's signature/integrity checks never run)
    //   WipeHeader    — zero PE headers post-map (also drops "we look like a
    //                   PE here" detection surface)
    //   NoSxS         — skip SxS activation context (we have no manifest)
    //   NoDelayLoad   — payload has no delay-loaded imports
    //
    // We do NOT pass NoThreads: thread-hijacking on V8's busy main thread
    // tends to deadlock. The default (CreateRemoteThread) is more reliable.
    int flags = blackbone::ManualImports
              | blackbone::WipeHeader
              | blackbone::NoSxS
              | blackbone::NoDelayLoad;

    auto map_result = proc.mmap().MapImage(opt.dll_absolute_path,
                                            static_cast<blackbone::eLoadFlags>(flags));
    if (!map_result.success()) {
        char buf[64];
        ::sprintf_s(buf, "0x%08lX", map_result.status);
        return Error{std::string{"blackbone::MapImage failed (NTSTATUS "} + buf + ")"};
    }

    return Success{ static_cast<uint64_t>(map_result.result()->baseAddress) };
}

} // namespace positron::injector
