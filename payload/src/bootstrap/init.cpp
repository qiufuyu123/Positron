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
    uint32_t pid = ::GetCurrentProcessId();
    log::init(pid);
    log::info("payload init starting");

    auto found = pe::find_exports_with("napi_register_module_v1");
    std::vector<std::string> symbols;
    if (found) {
        for (auto& kv : found->table.rva_by_name) {
            if (kv.first.rfind("napi_", 0) == 0) symbols.push_back(kv.first);
        }
    }

    ipc::Server::instance().set_greeter([symbols, pid]() -> wire::Json {
        wire::Hello h;
        h.electron_version = "";
        h.target_type = "";
        h.napi_symbols_present = symbols;
        h.pid = pid;
        return wire::encode_hello(h);
    });

    ipc::Server::instance().start(pid, [](const wire::Json& cmd) {
        log::info(std::string("recv cmd: ") + cmd.dump());
    });
    log::info("ipc server started");
    return 0;
}

} // namespace positron::bootstrap
