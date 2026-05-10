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
    char path[MAX_PATH];
    DWORD n = ::GetEnvironmentVariableA("TEMP", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    char full[MAX_PATH];
    _snprintf_s(full, MAX_PATH, "%s\\positron-payload.log", path);
    if (fopen_s(&f, full, "a") != 0) f = nullptr;
    return f;
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

} // namespace positron::log
