#include "pch.h"
#include "init.h"
#include "logging/log.h"
#include "pe_resolver/pe_resolver.h"
#include "ipc_server/ipc_server.h"
#include "wire/wire.h"
#include <Windows.h>
#include <vector>
#include <string>

namespace positron::bootstrap {

unsigned __stdcall init_thread_main(void*) {
    log::init(::GetCurrentProcessId());
    log::info("payload init starting");

    auto found = pe::find_exports_with("napi_register_module_v1");
    std::vector<std::string> symbols;
    if (found) {
        for (auto& kv : found->table.rva_by_name) {
            if (kv.first.rfind("napi_", 0) == 0) symbols.push_back(kv.first);
        }
    }

    ipc::Server::instance().start(::GetCurrentProcessId(), [](const wire::Json& cmd) {
        log::info(std::string("recv cmd: ") + cmd.dump());
    });

    for (int i = 0; i < 600 && !ipc::Server::instance().is_connected(); ++i) {
        ::Sleep(50);
    }
    if (ipc::Server::instance().is_connected()) {
        wire::Hello h;
        h.electron_version = "";
        h.target_type = "";
        h.napi_symbols_present = symbols;
        h.pid = ::GetCurrentProcessId();
        ipc::Server::instance().push(wire::encode_hello(h));
    }
    return 0;
}

} // namespace positron::bootstrap
