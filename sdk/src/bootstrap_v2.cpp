#include "bootstrap_v2.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace positron::sdk::bootstrap {

namespace {

std::string read_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return std::filesystem::path{buf}.parent_path().wstring();
}

} // anonymous

std::optional<BootstrapJs> load_bootstrap_v2_js() {
    // 1. POSITRON_BOOTSTRAP_JS_PATH env override.
    {
        wchar_t buf[1024] = {};
        DWORD n = ::GetEnvironmentVariableW(L"POSITRON_BOOTSTRAP_JS_PATH", buf, 1024);
        if (n > 0 && n < 1024) {
            std::wstring p{buf};
            auto s = read_file(p);
            if (!s.empty()) return BootstrapJs{ std::move(s), std::move(p) };
        }
    }
    // 2. <exe-dir>/bootstrap.js
    {
        auto dir = exe_dir();
        if (!dir.empty()) {
            std::wstring p = (std::filesystem::path{dir} / L"bootstrap.js").wstring();
            auto s = read_file(p);
            if (!s.empty()) return BootstrapJs{ std::move(s), std::move(p) };
        }
    }
    return std::nullopt;
}

} // namespace positron::sdk::bootstrap
