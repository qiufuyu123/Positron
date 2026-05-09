#include "pch.h"
#include "init.h"
#include "logging/log.h"
#include "pe_resolver/pe_resolver.h"
#include "ipc_server/ipc_server.h"
#include "hook_engine/hook_engine.h"
#include "napi_bridge/napi_bridge.h"
#include "js_executor/js_executor.h"
#include "wire/wire.h"
#include <Windows.h>
#include <vector>
#include <string>

namespace {
struct BridgeDispatcher : positron::js::IDispatcher {
    void run_on_v8_thread(std::function<void(napi_env)> fn) override {
        positron::napi::Bridge::instance().run_on_v8_thread(std::move(fn));
    }
    napi_env env_unsafe() override { return positron::napi::Bridge::instance().env_unsafe(); }
};
static BridgeDispatcher g_disp;
static positron::js::Executor g_exec(g_disp, positron::napi::Bridge::instance());
}

namespace positron::bootstrap {

unsigned __stdcall init_thread_main(void*) {
    uint32_t pid = ::GetCurrentProcessId();
    log::init(pid);
    log::info("payload init starting");

    hook::initialize();
    bool napi_ok = napi::Bridge::instance().initialize();

    std::vector<std::string> symbols = napi_ok
        ? napi::Bridge::instance().present_symbols
        : std::vector<std::string>{};

    ipc::Server::instance().set_greeter([symbols, pid]() -> wire::Json {
        wire::Hello h;
        h.electron_version = "";
        h.target_type = "";
        h.napi_symbols_present = symbols;
        h.pid = pid;
        return wire::encode_hello(h);
    });

    ipc::Server::instance().start(pid, [](const wire::Json& cmd) {
        auto kind = cmd.value("kind", std::string{});
        if (kind == "eval") {
            wire::EvalRequest req = wire::decode_eval_request(cmd);
            g_exec.eval(req, [](wire::EvalResponse resp) {
                ipc::Server::instance().push(wire::encode_eval_response(resp));
            });
        } else {
            log::warn("unknown cmd kind: " + kind);
        }
    });
    log::info("ipc server started");
    return 0;
}

} // namespace positron::bootstrap
