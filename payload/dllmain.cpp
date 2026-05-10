#include "pch.h"
#include "bootstrap/init.h"

// Manual-mapped DLL note: Blackbone's MapImage doesn't go through the Windows
// loader, so the CRT may have only partial state at DllMain time. To stay
// safe we:
//   1. emit a "milestone 0" via raw Win32 CreateEventW so we can confirm
//      DllMain ran even if std::* CRT facilities are wedged;
//   2. spawn the init thread via CreateThread (raw Win32) instead of
//      _beginthreadex (CRT) — _beginthreadex needs per-thread CRT init that
//      Blackbone may not have set up.

static DWORD WINAPI init_thread_trampoline(LPVOID) {
    return positron::bootstrap::init_thread_main(nullptr);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Milestone 0: DllMain reached. Encoded directly without CRT.
        wchar_t name[64];
        DWORD pid = ::GetCurrentProcessId();
        // hand-rolled wsprintf-equivalent without CRT swprintf:
        wchar_t* p = name;
        for (const wchar_t* s = L"Local\\positron-mile-"; *s; ++s) *p++ = *s;
        // unsigned int -> decimal
        wchar_t numbuf[16]; int ni = 0;
        unsigned u = pid;
        do { numbuf[ni++] = L'0' + (u % 10); u /= 10; } while (u);
        while (ni > 0) *p++ = numbuf[--ni];
        *p++ = L'-'; *p++ = L'0'; *p = 0;
        ::CreateEventW(nullptr, TRUE, TRUE, name);

        ::DisableThreadLibraryCalls(hModule);
        DWORD tid = 0;
        HANDLE h = ::CreateThread(nullptr, 0, init_thread_trampoline, nullptr, 0, &tid);
        if (h) ::CloseHandle(h);
    }
    return TRUE;
}
