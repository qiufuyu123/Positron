#include "pch.h"
#include "init.h"
#include "logging/log.h"
#include "pe_resolver/pe_resolver.h"
#include "ipc_server/ipc_server.h"
#include "hook_engine/hook_engine.h"
#include "v8_bridge/v8_bridge.h"
#include "teardown/teardown.h"
#include "wire/wire.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <cstdio>
#include <vector>
#include <string>

namespace positron::bootstrap {
std::atomic<bool> g_flusher_running{true};
} // namespace positron::bootstrap

// BlackBone residue addresses — filled when host sends teardown.info.
// Teardown uses these to RemoveVEH + VirtualFree the scratch pages.
namespace positron::teardown {
struct BlackboneResidueInfo {
    uint64_t veh_handle    = 0;
    uint64_t veh_code_addr = 0;
    uint64_t veh_code_size = 0;
    uint64_t mod_table_addr= 0;
    uint64_t mod_table_size= 0;
};
static BlackboneResidueInfo g_bb_info{};
void set_blackbone_info(const BlackboneResidueInfo& info) { g_bb_info = info; }
const BlackboneResidueInfo& get_blackbone_info() { return g_bb_info; }
} // namespace positron::teardown

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
        if (kind == "transport.select") {
            log::info("transport.select mode=" + cmd.value("mode", std::string{}));
            return;
        }
        if (kind == "teardown.info") {
            teardown::BlackboneResidueInfo info;
            info.veh_handle     = cmd.value("veh_handle",     uint64_t{0});
            info.veh_code_addr  = cmd.value("veh_code_addr",  uint64_t{0});
            info.veh_code_size  = cmd.value("veh_code_size",  uint64_t{0});
            info.mod_table_addr = cmd.value("mod_table_addr", uint64_t{0});
            info.mod_table_size = cmd.value("mod_table_size", uint64_t{0});
            teardown::set_blackbone_info(info);
            log::info("teardown.info received: veh_handle=" +
                      std::to_string(info.veh_handle) + " veh_code=" +
                      std::to_string(info.veh_code_addr) + " mod_table=" +
                      std::to_string(info.mod_table_addr));
            return;
        }
        if (kind == "bootstrap.start") {
            std::string code = cmd.value("code", std::string{});
            if (code.empty()) {
                ipc::Server::instance().push(wire::Json{
                    {"kind", "v2.error"}, {"message", "empty bootstrap code"}});
                return;
            }
            // Run the bootstrap in the V8 main thread. The IIFE returns
            // immediately; server.listen() finishes asynchronously, so we
            // poll globalThis.__positron_v2 for readiness.
            v8b::V8Bridge::instance().eval_async(code, [](v8b::V8Bridge::EvalOutcome out) {
                if (!out.ok) {
                    ipc::Server::instance().push(wire::Json{
                        {"kind", "v2.error"},
                        {"message", "bootstrap eval failed: " + out.error_message},
                        {"stack",   out.error_stack}});
                    return;
                }
                // Spin a poll thread; it'll send v2.ready or v2.error to host.
                std::thread([]{
                    using clock = std::chrono::steady_clock;
                    auto deadline = clock::now() + std::chrono::seconds(5);
                    const std::string check_js =
                        "(function(){for(var k in globalThis){if(k[0]==='_'&&globalThis[k]&&globalThis[k].__positron_status)return globalThis[k]}return null;})()";
                    while (clock::now() < deadline) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(150));
                        std::promise<v8b::V8Bridge::EvalOutcome> prom;
                        auto fut = prom.get_future();
                        bool sched = v8b::V8Bridge::instance().eval_async(
                            check_js, [&prom](auto o){ try { prom.set_value(std::move(o)); } catch(...){} });
                        if (!sched) continue;
                        if (fut.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready) continue;
                        auto o = fut.get();
                        if (!o.ok) continue;
                        // out.json_value is JSON.stringify of the wrapper's value field.
                        // null  -> still pending; otherwise it's an object.
                        if (o.json_value.empty() || o.json_value == "null") continue;
                        try {
                            auto j = nlohmann::json::parse(o.json_value);
                            auto status = j.value("__positron_status", std::string{});
                            if (status == "ready") {
                                int port = j.value("port", 0);
                                ipc::Server::instance().push(wire::Json{
                                    {"kind", "v2.ready"}, {"port", port}});
                                return;
                            }
                            if (status == "error") {
                                ipc::Server::instance().push(wire::Json{
                                    {"kind", "v2.error"},
                                    {"message", j.value("message", std::string{"unknown"})},
                                    {"stack",   j.value("stack",   std::string{})}});
                                return;
                            }
                        } catch (...) { /* keep polling */ }
                    }
                    ipc::Server::instance().push(wire::Json{
                        {"kind", "v2.error"},
                        {"message", "bootstrap timeout: __positron_v2 never reached ready"}});
                }).detach();
            });
            return;
        }
        if (kind == "v2.commit") {
            // Host has switched to the JS server. We're now redundant -- the
            // JS server in V8 owns the live REPL. Tear ourselves down:
            //   1. Set shutdown flags so detached threads (hit-flusher,
            //      renderer-poller) bail out on their next tick.
            //   2. request_stop on the ipc server so accept_thread + writer
            //      exit naturally as their socket closes (we're inside
            //      accept_thread right now -- can't join self).
            //   3. Schedule the self-unmap trampoline on a kernel32-resident
            //      scratch page; it sleeps 500 ms (lets all our threads
            //      fully exit) then VirtualFree's our image base.
            log::info("v2.commit: tearing down payload (phase 2 self-unmap)");
            g_flusher_running.store(false, std::memory_order_release);
            v8b::V8Bridge::instance().request_poller_shutdown();
            ipc::Server::instance().request_stop();
            teardown::schedule_unmap(/*delay_ms=*/500);
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
            auto cb = [id](v8b::V8Bridge::EvalOutcome out) {
                wire::EvalResponse resp;
                resp.id = id;
                resp.ok = out.ok;
                if (out.ok) {
                    resp.result = wire::EvalResult{ out.type_tag, out.json_value };
                } else {
                    resp.error = wire::EvalError{ out.error_message, out.error_stack };
                }
                ipc::Server::instance().push(wire::encode_eval_response(resp));
            };

            bool sched;
            if (req.world == "renderer") {
                int idx = req.world_index.value_or(0);
                sched = v8b::V8Bridge::instance().eval_renderer_async(req.code, idx, cb);
            } else {
                sched = v8b::V8Bridge::instance().eval_async(req.code, cb);
            }
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
    std::thread([]{
        while (g_flusher_running.load()) {
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
