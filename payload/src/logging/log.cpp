#include "pch.h"
#include "log.h"
#include <Windows.h>
#include <atomic>
#include <mutex>
#include <cstdio>

namespace positron::log {

static std::atomic<ForwardFn> g_forwarder{nullptr};
static uint32_t g_pid = 0;
static std::mutex g_file_mu;

static FILE* open_log_file() {
    static FILE* f = nullptr;
    static bool tried = false;
    if (tried) return f;
    tried = true;
    // Try several locations in priority order. Chromium-sandboxed (Low IL)
    // renderer processes cannot write to %TEMP% (Medium IL), but C:\Users\Public
    // is world-writable on default installs and accessible from any IL.
    const char* candidates[] = {
        "C:\\Users\\Public\\positron-payload.log",
        nullptr, // %TEMP%
        nullptr, // %LOCALAPPDATA%
    };
    char tmp_buf[MAX_PATH] = {};
    char loc_buf[MAX_PATH] = {};
    if (::GetEnvironmentVariableA("TEMP", tmp_buf, MAX_PATH)) {
        static char tmp_full[MAX_PATH];
        _snprintf_s(tmp_full, MAX_PATH, "%s\\positron-payload.log", tmp_buf);
        candidates[1] = tmp_full;
    }
    if (::GetEnvironmentVariableA("LOCALAPPDATA", loc_buf, MAX_PATH)) {
        static char loc_full[MAX_PATH];
        _snprintf_s(loc_full, MAX_PATH, "%s\\positron-payload.log", loc_buf);
        candidates[2] = loc_full;
    }
    for (auto* path : candidates) {
        if (!path) continue;
        if (fopen_s(&f, path, "a") == 0 && f) return f;
    }
    return nullptr;
}

static void emit(const char* level, std::string s) {
    DWORD t = ::GetTickCount();
    std::string line = "[t=" + std::to_string(t) + "ms positron pid=" + std::to_string(g_pid) + " " + level + "] " + s + "\n";
    ::OutputDebugStringA(line.c_str());
    {
        std::lock_guard lk(g_file_mu);
        if (FILE* f = open_log_file()) {
            std::fputs(line.c_str(), f);
            std::fflush(f);
        }
    }
    if (auto f = g_forwarder.load()) f(level, s);
}

void init(uint32_t pid) { g_pid = pid; }
void info(std::string s)  { emit("info",  std::move(s)); }
void warn(std::string s)  { emit("warn",  std::move(s)); }
void error(std::string s) { emit("error", std::move(s)); }
void set_forwarder(ForwardFn f) { g_forwarder.store(f); }

void milestone(int n) {
    wchar_t name[64];
    swprintf_s(name, L"Local\\positron-mile-%u-%d", g_pid, n);
    // Manual-reset, initially signaled. Handle is intentionally leaked — its
    // lifetime is the payload's lifetime, which is fine.
    ::CreateEventW(nullptr, TRUE, TRUE, name);
}

} // namespace positron::log
