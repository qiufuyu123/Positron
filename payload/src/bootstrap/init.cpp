#include "pch.h"
#include "init.h"
#include "logging/log.h"
#include "pe_resolver/pe_resolver.h"
#include <Windows.h>

namespace positron::bootstrap {

unsigned __stdcall init_thread_main(void*) {
    log::init(::GetCurrentProcessId());
    log::info("payload init starting");

    auto found = pe::find_exports_with("napi_register_module_v1");
    if (!found) {
        log::warn("no napi_register_module_v1 export found in known modules; payload running with reduced capability");
    } else {
        log::info("found napi exports in module (count=" + std::to_string(found->table.rva_by_name.size()) + ")");
    }

    return 0;
}

} // namespace positron::bootstrap
