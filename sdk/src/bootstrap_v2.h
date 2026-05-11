#pragma once
#include <optional>
#include <string>

namespace positron::sdk::bootstrap {

struct BootstrapJs {
    std::string source;
    std::wstring resolved_path;   // for diagnostics
};

// Resolve and read the v2 bootstrap JS from disk. Lookup order:
//   1. POSITRON_BOOTSTRAP_JS_PATH env (absolute path)
//   2. <executable-dir>/bootstrap.js  (msbuild Copy step lands it here)
//
// Returns nullopt if neither path resolves to a non-empty file. The SDK
// surfaces this as an AttachError so the caller knows to install the file
// or set the env override.
std::optional<BootstrapJs> load_bootstrap_v2_js();

} // namespace positron::sdk::bootstrap
