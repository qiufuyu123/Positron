#include "pch.h"
#include "teardown.h"
#include "logging/log.h"
#include "ipc_server/ipc_server.h"
#include "v8_bridge/v8_bridge.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

// Linker-provided symbol pointing to the load address of THIS module.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace positron::teardown {

namespace {

#if defined(_M_X64)

struct TeardownData {
    void*    base;              // [+0]
    void*    kSleep;            // [+8]
    void*    kVirtualFree;      // [+16]
    void*    kExitThread;       // [+24]
    uint64_t delay_ms;          // [+32]
    void*    scratch_base;      // [+40]  address of the scratch page itself
    void*    kVirtualProtect;   // [+48]  kernel32!VirtualProtect
};

// x64 trampoline:
//   sub rsp, 0x38              ; shadow + align + space for VirtualProtect &oldProt
//   mov rbx, rcx               ; save TeardownData*
//   mov ecx, [rbx+32]          ; Sleep(delay_ms)
//   mov rax, [rbx+8]
//   call rax
//   mov rcx, [rbx+0]           ; VirtualFree(payload_base, 0, MEM_RELEASE)
//   xor edx, edx
//   mov r8d, 0x8000
//   mov rax, [rbx+16]
//   call rax
//   mov rcx, [rbx+40]          ; VirtualProtect(scratch, 0x1000, PAGE_READONLY=2, &old)
//   mov edx, 0x1000
//   mov r8d, 2
//   lea r9, [rsp+0x30]         ; &oldProt on stack
//   mov rax, [rbx+48]
//   call rax
//   xor ecx, ecx               ; ExitThread(0)
//   mov rax, [rbx+24]
//   call rax
//   ud2
constexpr uint8_t kTrampoline[] = {
    0x48, 0x83, 0xEC, 0x38,         // sub rsp, 0x38
    0x48, 0x89, 0xCB,               // mov rbx, rcx
    0x8B, 0x4B, 0x20,               // mov ecx, [rbx+32]
    0x48, 0x8B, 0x43, 0x08,         // mov rax, [rbx+8]
    0xFF, 0xD0,                     // call rax (Sleep)
    0x48, 0x8B, 0x0B,               // mov rcx, [rbx]
    0x31, 0xD2,                     // xor edx, edx
    0x41, 0xB8, 0x00, 0x80, 0x00, 0x00, // mov r8d, 0x8000
    0x48, 0x8B, 0x43, 0x10,         // mov rax, [rbx+16]
    0xFF, 0xD0,                     // call rax (VirtualFree payload)
    0x48, 0x8B, 0x4B, 0x28,         // mov rcx, [rbx+40] (scratch_base)
    0xBA, 0x00, 0x10, 0x00, 0x00,   // mov edx, 0x1000
    0x41, 0xB8, 0x02, 0x00, 0x00, 0x00, // mov r8d, 2 (PAGE_READONLY)
    0x4C, 0x8D, 0x4C, 0x24, 0x30,  // lea r9, [rsp+0x30]
    0x48, 0x8B, 0x43, 0x30,         // mov rax, [rbx+48] (VirtualProtect)
    0xFF, 0xD0,                     // call rax
    0x31, 0xC9,                     // xor ecx, ecx
    0x48, 0x8B, 0x43, 0x18,         // mov rax, [rbx+24]
    0xFF, 0xD0,                     // call rax (ExitThread)
    0x0F, 0x0B,                     // ud2
};

#elif defined(_M_IX86)

struct TeardownData {
    void*    base;              // [+0]
    void*    kSleep;            // [+4]
    void*    kVirtualFree;      // [+8]
    void*    kExitThread;       // [+12]
    uint32_t delay_ms;          // [+16]
    void*    scratch_base;      // [+20]
    void*    kVirtualProtect;   // [+24]
};

// x86 trampoline:
//   push esi
//   mov esi, [esp+8]
//   push [esi+16]        ; Sleep(delay_ms)
//   call [esi+4]
//   push 0x8000           ; VirtualFree(base, 0, MEM_RELEASE)
//   push 0
//   push [esi]
//   call [esi+8]
//   lea eax, [esp]        ; &oldProt (reuse stack)
//   push eax              ; lpflOldProtect
//   push 2                ; PAGE_READONLY
//   push 0x1000           ; size
//   push [esi+20]         ; scratch_base
//   call [esi+24]         ; VirtualProtect
//   push 0                ; ExitThread(0)
//   call [esi+12]
//   ud2
constexpr uint8_t kTrampoline[] = {
    0x56,                           // push esi
    0x8B, 0x74, 0x24, 0x08,         // mov esi, [esp+8]
    0xFF, 0x76, 0x10,               // push [esi+16]
    0xFF, 0x56, 0x04,               // call [esi+4] (Sleep)
    0x68, 0x00, 0x80, 0x00, 0x00,   // push 0x8000
    0x6A, 0x00,                     // push 0
    0xFF, 0x36,                     // push [esi]
    0xFF, 0x56, 0x08,               // call [esi+8] (VirtualFree payload)
    0x8D, 0x04, 0x24,               // lea eax, [esp]
    0x50,                           // push eax (&oldProt)
    0x6A, 0x02,                     // push 2 (PAGE_READONLY)
    0x68, 0x00, 0x10, 0x00, 0x00,   // push 0x1000
    0xFF, 0x76, 0x14,               // push [esi+20] (scratch_base)
    0xFF, 0x56, 0x18,               // call [esi+24] (VirtualProtect)
    0x6A, 0x00,                     // push 0
    0xFF, 0x56, 0x0C,               // call [esi+12] (ExitThread)
    0x0F, 0x0B,                     // ud2
};

#else
#error "unsupported architecture"
#endif

} // anonymous

// BlackBone residue info — declared in init.cpp, defined in teardown
// namespace. Forward-declare the accessor here.
struct BlackboneResidueInfo {
    uint64_t veh_handle    = 0;
    uint64_t veh_code_addr = 0;
    uint64_t veh_code_size = 0;
    uint64_t mod_table_addr= 0;
    uint64_t mod_table_size= 0;
};
const BlackboneResidueInfo& get_blackbone_info();

bool schedule_unmap(uint32_t delay_ms) {
    log::info("teardown: arming unmap trampoline (delay=" +
              std::to_string(delay_ms) + "ms)");

    // 1. Stop the detached threads cooperatively.
    v8b::V8Bridge::instance().request_poller_shutdown();

    // 1b. Remove BlackBone's VEH and free its scratch pages. This must
    //     happen while our code is still alive (before the trampoline
    //     frees our .text) because RemoveVectoredExceptionHandler runs
    //     kernel-side synchronously and needs the handle to be valid.
    {
        auto& bb = get_blackbone_info();
        if (bb.veh_handle) {
            ULONG removed = ::RemoveVectoredExceptionHandler(
                reinterpret_cast<PVOID>(bb.veh_handle));
            log::info("teardown: RemoveVEH handle=" +
                      std::to_string(bb.veh_handle) +
                      " result=" + std::to_string(removed));
        }
        if (bb.veh_code_addr) {
            ::VirtualFree(reinterpret_cast<LPVOID>(bb.veh_code_addr), 0, MEM_RELEASE);
            log::info("teardown: freed VEH code page at " +
                      std::to_string(bb.veh_code_addr));
        }
        if (bb.mod_table_addr) {
            ::VirtualFree(reinterpret_cast<LPVOID>(bb.mod_table_addr), 0, MEM_RELEASE);
            log::info("teardown: freed mod table at " +
                      std::to_string(bb.mod_table_addr));
        }
    }

    // 2. Resolve the kernel32 imports we need to call WITHOUT going through
    //    our (about-to-vanish) IAT.
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!k32) { log::error("teardown: kernel32 GetModuleHandle failed"); return false; }
    auto pSleep          = reinterpret_cast<void*>(::GetProcAddress(k32, "Sleep"));
    auto pVirtualFree    = reinterpret_cast<void*>(::GetProcAddress(k32, "VirtualFree"));
    auto pExitThread     = reinterpret_cast<void*>(::GetProcAddress(k32, "ExitThread"));
    auto pVirtualProtect = reinterpret_cast<void*>(::GetProcAddress(k32, "VirtualProtect"));
    if (!pSleep || !pVirtualFree || !pExitThread || !pVirtualProtect) {
        log::error("teardown: kernel32 GetProcAddress failed");
        return false;
    }

    // 3. Allocate a scratch RWX page. Lives at a separate VA from our image,
    //    so it's not affected when we VirtualFree our base.
    void* scratch = ::VirtualAlloc(nullptr, 0x1000,
                                   MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
    if (!scratch) { log::error("teardown: VirtualAlloc scratch failed"); return false; }

    // Layout: shellcode at [scratch+0], TeardownData at [scratch+0x100].
    auto* code = static_cast<uint8_t*>(scratch);
    auto* data = reinterpret_cast<TeardownData*>(code + 0x100);

    std::memcpy(code, kTrampoline, sizeof(kTrampoline));

    data->base            = static_cast<void*>(&__ImageBase);
    data->kSleep          = pSleep;
    data->kVirtualFree    = pVirtualFree;
    data->kExitThread     = pExitThread;
    data->delay_ms        = delay_ms;
    data->scratch_base    = scratch;
    data->kVirtualProtect = pVirtualProtect;

    // CPU may have cached our recent writes; flush so the new thread sees
    // executable code rather than stale (we just allocated, so unlikely
    // an issue, but cheap insurance).
    ::FlushInstructionCache(::GetCurrentProcess(), scratch, sizeof(kTrampoline));

    // 4. Spawn the trampoline thread. It Sleep()s, then VirtualFree's our
    //    base, then ExitThread's. Never returns to any code in our pages.
    HANDLE th = ::CreateThread(
        nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(scratch),
        data, 0, nullptr);
    if (!th) {
        log::error("teardown: CreateThread trampoline failed: " +
                   std::to_string(::GetLastError()));
        ::VirtualFree(scratch, 0, MEM_RELEASE);
        return false;
    }
    ::CloseHandle(th);
    log::info("teardown: trampoline scheduled at " +
              std::to_string(reinterpret_cast<uintptr_t>(scratch)) +
              " for base " +
              std::to_string(reinterpret_cast<uintptr_t>(&__ImageBase)));
    return true;
}

} // namespace positron::teardown
