#include <positron/process.h>
#include "process_enum/process_enum.h"

namespace positron::sdk {

const wchar_t* type_name(ProcessType t) {
    switch (t) {
        case ProcessType::Main:     return L"main";
        case ProcessType::Renderer: return L"renderer";
        case ProcessType::Gpu:      return L"gpu";
        case ProcessType::Utility:  return L"utility";
        default:                    return L"unknown";
    }
}

static ProcessType to_sdk(positron::proc::ElectronType t) {
    switch (t) {
        case positron::proc::ElectronType::Main:     return ProcessType::Main;
        case positron::proc::ElectronType::Renderer: return ProcessType::Renderer;
        case positron::proc::ElectronType::Gpu:      return ProcessType::Gpu;
        case positron::proc::ElectronType::Utility:  return ProcessType::Utility;
        default:                                     return ProcessType::Unknown;
    }
}

std::vector<Process> list_processes() {
    std::vector<Process> out;
    for (auto& e : positron::proc::enumerate_electron()) {
        Process p;
        p.pid          = e.pid;
        p.parent_pid   = e.parent_pid;
        p.exe_name     = e.exe_name;
        p.full_path    = e.full_path;
        p.command_line = e.command_line;
        p.type         = to_sdk(e.type);
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace positron::sdk
