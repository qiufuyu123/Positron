#include "injector.h"
#include <Windows.h>

// Blackbone — manual DLL mapping. Bypasses the standard Windows loader so
// the payload can be planted into sandboxed renderer processes that block
// LoadLibrary on unsigned/non-system DLLs.
#include <BlackBone/Process/Process.h>
#include <BlackBone/ManualMap/MMap.h>

namespace positron::injector {

// Target arch must match host arch — Blackbone manual mapping cannot
// cross-bitness inject. So x64 host accepts only x64 targets, and x86 host
// only x86 targets.
bool target_is_x64(uint32_t pid, std::string& err) {
#ifdef _M_X64
    constexpr USHORT kHostMachine = IMAGE_FILE_MACHINE_AMD64;
    const char* host_arch_name = "x64";
#elif defined(_M_IX86)
    constexpr USHORT kHostMachine = IMAGE_FILE_MACHINE_I386;
    const char* host_arch_name = "x86";
#elif defined(_M_ARM64)
    constexpr USHORT kHostMachine = IMAGE_FILE_MACHINE_ARM64;
    const char* host_arch_name = "ARM64";
#else
    #error "unsupported host architecture"
#endif

    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { err = "OpenProcess failed: " + std::to_string(::GetLastError()); return false; }
    USHORT proc = 0, mach = 0;
    BOOL ok = ::IsWow64Process2(h, &proc, &mach);
    ::CloseHandle(h);
    if (!ok) { err = "IsWow64Process2 failed: " + std::to_string(::GetLastError()); return false; }
    USHORT effective = (proc == IMAGE_FILE_MACHINE_UNKNOWN) ? mach : proc;
    if (effective == kHostMachine) return true;

    const char* arch = "unknown";
    switch (effective) {
        case IMAGE_FILE_MACHINE_AMD64: arch = "x64"; break;
        case IMAGE_FILE_MACHINE_I386:  arch = "x86 (32-bit)"; break;
        case IMAGE_FILE_MACHINE_ARM64: arch = "ARM64"; break;
        case IMAGE_FILE_MACHINE_ARM:   arch = "ARM (32-bit)"; break;
        case IMAGE_FILE_MACHINE_ARMNT: arch = "ARM Thumb-2"; break;
        default: break;
    }
    char b[8]; ::sprintf_s(b, "%X", effective);
    err = std::string{"target is "} + arch + " (machine=0x" + b +
          "); this is the " + host_arch_name + " positron — use the matching arch host.exe.";
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

    auto& em = proc.mmap().exceptMgr();
    Success s{};
    s.module_base   = static_cast<uint64_t>(map_result.result()->baseAddress);
    s.veh_handle    = em.vehHandle();
    s.veh_code_addr = static_cast<uint64_t>(em.vehCodePtr());
    s.veh_code_size = static_cast<uint64_t>(em.vehCodeSize());
    s.mod_table_addr= static_cast<uint64_t>(em.modTablePtr());
    s.mod_table_size= static_cast<uint64_t>(em.modTableSize());
    return s;
}

} // namespace positron::injector
