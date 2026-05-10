#include "pch.h"
#include "init.h"
#include "logging/log.h"
#include "pe_resolver/pe_resolver.h"
#include "ipc_server/ipc_server.h"
#include "hook_engine/hook_engine.h"
#include "v8_bridge/v8_bridge.h"
#include "wire/wire.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
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
    bool v8_ok = v8b::V8Bridge::instance().initialize();
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
        if (kind == "detach") {
            log::info("detach: disabling hooks + closing connection");
            hook::shutdown();
            ipc::Server::instance().stop();
            return;
        }
        if (kind == "hook.install") {
            auto req = wire::decode_hook_install(cmd);
            // Find any module that exports the requested symbol.
            auto found = pe::find_exports_with(req.symbol);
            if (!found) {
                log::warn("hook.install: symbol not found in any module: " + req.symbol);
                return;
            }
            auto addr = found->table.abs(req.symbol);
            if (!addr) {
                log::warn("hook.install: resolve failed: " + req.symbol);
                return;
            }
            int slot = hook::install_user_hook(req.id, reinterpret_cast<void*>(*addr));
            log::info("hook.install id=" + std::to_string(req.id)
                      + " sym=" + req.symbol
                      + " slot=" + std::to_string(slot));
            return;
        }
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

    // Hit-flusher: periodically drain the hook ring buffer and emit hook.hit
    // events. Detaches because the payload outlives this thread.
    static std::atomic<bool> flusher_running{true};
    std::thread([]{
        while (flusher_running.load()) {
            hook::UserHit hit{};
            uint32_t dropped = 0;
            while (hook::drain_user_hit(&hit, &dropped)) {
                wire::HookHitEvent ev;
                ev.hook_id = hit.hook_id;
                for (int i = 0; i < 4; ++i) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "\"0x%llx\"",
                                  static_cast<unsigned long long>(hit.args[i]));
                    ev.args_json.push_back(buf);
                }
                ev.dropped_since_last = dropped;
                ipc::Server::instance().push(wire::encode_hook_hit(ev));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();

    return 0;
}

} // namespace positron::bootstrap
