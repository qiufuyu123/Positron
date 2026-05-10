#include "pch.h"
#include "init.h"
#include "logging/log.h"
#include "pe_resolver/pe_resolver.h"
#include "ipc_server/ipc_server.h"
#include "hook_engine/hook_engine.h"
#include "v8_bridge/v8_bridge.h"
#include "wire/wire.h"
#include <Windows.h>
#include <vector>
#include <string>

namespace positron::bootstrap {

// Lightweight, side-effect-free scan of napi_* exports for HELLO diagnostics.
// Avoids pulling in napi_bridge (which historically installed LoadLibrary
// hooks + a polling watcher thread that contends with the V8 main thread
// during Electron startup, freezing the UI).
static std::vector<std::string> scan_napi_symbols() {
    std::vector<std::string> out;
    auto found = pe::find_exports_with("napi_run_script");
    if (!found) return out;
    for (auto& kv : found->table.rva_by_name) {
        if (kv.first.rfind("napi_", 0) == 0) out.push_back(kv.first);
    }
    return out;
}

unsigned __stdcall init_thread_main(void*) {
    uint32_t pid = ::GetCurrentProcessId();
    log::init(pid);
    log::info("payload init starting");

    hook::initialize();

    // v8_bridge is the primary execution path: it resolves V8 + libuv exports
    // (mangled C++ symbols + plain uv_*) and arms a uv_async_t on the default
    // loop. Works without any addon being loaded.
    bool v8_ok = v8b::V8Bridge::instance().initialize();

    // napi symbol presence is gathered passively (no hooks installed).
    std::vector<std::string> symbols = scan_napi_symbols();

    ipc::Server::instance().set_greeter([symbols, pid, v8_ok]() -> wire::Json {
        wire::Hello h;
        h.electron_version = v8_ok ? "v8_bridge_ready" : "v8_bridge_not_ready";
        h.target_type = "";
        h.napi_symbols_present = symbols;
        h.pid = pid;
        return wire::encode_hello(h);
    });

    ipc::Server::instance().start(pid, [](const wire::Json& cmd) {
        auto kind = cmd.value("kind", std::string{});
        log::info("ipc recv kind=" + kind);
        if (kind == "eval") {
            wire::EvalRequest req = wire::decode_eval_request(cmd);
            uint64_t id = req.id;
            bool sched = v8b::V8Bridge::instance().eval_async(req.code,
                [id](v8b::V8Bridge::EvalOutcome out) {
                    wire::EvalResponse resp;
                    resp.id = id;
                    resp.ok = out.ok;
                    if (out.ok) {
                        resp.result = wire::EvalResult{ out.type_tag, out.json_value };
                    } else {
                        resp.error = wire::EvalError{ out.error_message, out.error_stack };
                    }
                    ipc::Server::instance().push(wire::encode_eval_response(resp));
                });
            if (!sched) {
                wire::EvalResponse resp;
                resp.id = req.id;
                resp.ok = false;
                resp.error = wire::EvalError{ "v8_bridge not ready", "" };
                ipc::Server::instance().push(wire::encode_eval_response(resp));
            }
        } else {
            log::warn("unknown cmd kind: " + kind);
        }
    });
    log::info("ipc server started");
    return 0;
}

} // namespace positron::bootstrap
