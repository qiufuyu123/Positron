#include "pch.h"
#include "bootstrap/init.h"
#include <process.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(hModule);
        unsigned tid = 0;
        HANDLE h = reinterpret_cast<HANDLE>(_beginthreadex(
            nullptr, 0, &positron::bootstrap::init_thread_main, nullptr, 0, &tid));
        if (h) ::CloseHandle(h);
    }
    return TRUE;
}
