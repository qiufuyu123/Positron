#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace positron::sdk {

enum class ProcessType {
    Unknown,
    Main,       // Electron main process (no --type=)
    Renderer,   // --type=renderer
    Gpu,        // --type=gpu-process
    Utility,    // --type=utility / etc.
};

struct Process {
    uint32_t     pid         = 0;
    uint32_t     parent_pid  = 0;
    std::wstring exe_name;        // basename, e.g. "Code.exe"
    std::wstring full_path;       // absolute path to the exe
    std::wstring command_line;    // empty if not readable
    ProcessType  type         = ProcessType::Unknown;
};

// Friendly name for ProcessType (e.g. L"main", L"renderer").
const wchar_t* type_name(ProcessType);

// Enumerate Chromium / Electron-family processes on the local machine.
// Identifies them by the canonical Chromium fingerprint:
//   * any process whose command line contains "--type="
//   * plus its parent if the parent's exe matches (Chromium reuses one
//     binary for all process roles).
//
// Note: cross-bitness reads (x86 host querying x64 targets, or vice versa)
// fail silently — those processes simply don't appear. Use a host of the
// matching arch to enumerate the targets you care about.
std::vector<Process> list_processes();

} // namespace positron::sdk
