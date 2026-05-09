#include "pch.h"
#include "log.h"
#include <Windows.h>
#include <atomic>

namespace positron::log {

static std::atomic<ForwardFn> g_forwarder{nullptr};
static uint32_t g_pid = 0;

static void emit(const char* level, std::string s) {
    std::string line = "[positron pid=" + std::to_string(g_pid) + " " + level + "] " + s + "\n";
    ::OutputDebugStringA(line.c_str());
    if (auto f = g_forwarder.load()) f(level, s);
}

void init(uint32_t pid) { g_pid = pid; }
void info(std::string s)  { emit("info",  std::move(s)); }
void warn(std::string s)  { emit("warn",  std::move(s)); }
void error(std::string s) { emit("error", std::move(s)); }
void set_forwarder(ForwardFn f) { g_forwarder.store(f); }

} // namespace positron::log
