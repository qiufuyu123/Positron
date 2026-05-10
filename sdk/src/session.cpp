#include <positron/session.h>

#include "injector/injector.h"
#include "pipe_client/pipe_client.h"
#include <wire/wire.h>

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace positron::sdk {

namespace {

std::wstring default_payload_path() {
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (std::filesystem::path{buf}.parent_path() / L"payload.dll").wstring();
}

const char* world_to_wire(World w) {
    switch (w) {
        case World::Renderer: return "renderer";
        case World::Node:     return "node";
        case World::Auto:
        default:              return "auto";
    }
}

EvalResult outcome_to_result(const wire::EvalResponse& resp) {
    EvalResult r;
    r.ok = resp.ok;
    if (resp.ok && resp.result) {
        r.type_tag   = resp.result->type_tag;
        r.json_value = resp.result->json;
    } else if (resp.error) {
        r.error_message = resp.error->message;
        r.error_stack   = resp.error->stack;
    }
    return r;
}

} // anonymous

// =============================================================================
// Session::Impl
// =============================================================================

struct Session::Impl {
    pipe::Client          conn;
    std::thread           reader;
    std::atomic<bool>     reader_running{false};
    std::atomic<uint64_t> next_id{1};

    std::mutex                                            pending_mu;
    std::unordered_map<uint64_t, std::promise<EvalResult>> pending;

    std::mutex                pending_async_mu;
    std::unordered_map<uint64_t, std::function<void(EvalResult)>> pending_async;

    std::mutex      hook_mu;
    HookHitHandler  hook_handler;

    ~Impl() { stop(); }

    void start_reader() {
        reader_running = true;
        reader = std::thread([this]{ this->run_reader(); });
    }

    void stop() {
        reader_running = false;
        conn.close();
        if (reader.joinable()) reader.join();

        // Fail any waiters so callers don't hang.
        {
            std::lock_guard<std::mutex> lk(pending_mu);
            for (auto& kv : pending) {
                EvalResult r;
                r.ok = false;
                r.error_message = "session detached";
                try { kv.second.set_value(std::move(r)); } catch (...) {}
            }
            pending.clear();
        }
        {
            std::lock_guard<std::mutex> lk(pending_async_mu);
            for (auto& kv : pending_async) {
                EvalResult r;
                r.ok = false;
                r.error_message = "session detached";
                try { kv.second(std::move(r)); } catch (...) {}
            }
            pending_async.clear();
        }
    }

    void run_reader() {
        while (reader_running.load()) {
            auto msg = conn.recv(200);
            if (!msg) {
                if (!conn.is_open()) break;
                continue;
            }
            auto kind = msg->value("kind", std::string{});
            if (kind == "eval.response") {
                auto resp = wire::decode_eval_response(*msg);
                EvalResult r = outcome_to_result(resp);

                bool delivered = false;
                {
                    std::lock_guard<std::mutex> lk(pending_mu);
                    auto it = pending.find(resp.id);
                    if (it != pending.end()) {
                        try { it->second.set_value(std::move(r)); } catch (...) {}
                        pending.erase(it);
                        delivered = true;
                    }
                }
                if (!delivered) {
                    std::function<void(EvalResult)> cb;
                    {
                        std::lock_guard<std::mutex> lk(pending_async_mu);
                        auto it = pending_async.find(resp.id);
                        if (it != pending_async.end()) {
                            cb = std::move(it->second);
                            pending_async.erase(it);
                        }
                    }
                    if (cb) try { cb(std::move(r)); } catch (...) {}
                }
            } else if (kind == "hook.hit") {
                auto ev = wire::decode_hook_hit(*msg);
                HookHit h;
                h.hook_id = ev.hook_id;
                h.dropped_since_last = ev.dropped_since_last;
                for (auto& s : ev.args_json) {
                    // args_json items are strings like "0x...".
                    uint64_t v = 0;
                    try { v = std::stoull(s, nullptr, 0); }
                    catch (...) {
                        // Strip quotes if present, then retry.
                        std::string trim = s;
                        if (trim.size() >= 2 && trim.front() == '"' && trim.back() == '"')
                            trim = trim.substr(1, trim.size() - 2);
                        try { v = std::stoull(trim, nullptr, 0); } catch (...) { v = 0; }
                    }
                    h.args.push_back(v);
                }
                HookHitHandler cb;
                {
                    std::lock_guard<std::mutex> lk(hook_mu);
                    cb = hook_handler;
                }
                if (cb) try { cb(h); } catch (...) {}
            }
            // Other kinds (ready, log, …) are silently ignored at SDK level for
            // now — surface them via a future on_event() if needed.
        }
    }
};

// =============================================================================
// Session — public
// =============================================================================

Session::Session() : p(std::make_unique<Impl>()) {}
Session::~Session() = default;
Session::Session(Session&&) noexcept            = default;
Session& Session::operator=(Session&&) noexcept = default;

std::optional<Session::AttachError>
Session::attach(uint32_t pid, const std::wstring& payload_dll) {
    if (!p) p = std::make_unique<Impl>();

    std::wstring dll = payload_dll.empty() ? default_payload_path() : payload_dll;

    auto rr = positron::injector::inject({pid, dll, 10000});
    if (auto* e = std::get_if<positron::injector::Error>(&rr)) {
        return AttachError{e->message};
    }

    auto cr = p->conn.connect(pid, 5000);
    if (auto* e = std::get_if<positron::pipe::ConnectError>(&cr)) {
        return AttachError{e->message};
    }

    // Drain the HELLO frame so subsequent recv()s in the reader get the
    // command/response stream cleanly.
    auto hello = p->conn.recv(5000);
    if (!hello) return AttachError{"no HELLO frame received from payload"};

    p->start_reader();
    return std::nullopt;
}

void Session::detach() {
    if (!p) return;
    if (p->conn.is_open()) {
        wire::Json detach{{"kind", "detach"}};
        p->conn.send(detach);
    }
    p->stop();
}

bool Session::is_connected() const {
    return p && p->conn.is_open();
}

EvalResult Session::eval(const std::string& code, const EvalOptions& opts) {
    EvalResult fail;
    fail.ok = false;

    if (!p || !p->conn.is_open()) {
        fail.error_message = "not connected";
        return fail;
    }

    uint64_t id = p->next_id.fetch_add(1, std::memory_order_relaxed);

    std::promise<EvalResult>     prom;
    std::future<EvalResult>      fut = prom.get_future();
    {
        std::lock_guard<std::mutex> lk(p->pending_mu);
        p->pending.emplace(id, std::move(prom));
    }

    wire::EvalRequest req;
    req.id   = id;
    req.code = code;
    req.world = world_to_wire(opts.world);
    if (opts.world == World::Renderer) req.world_index = opts.window_index;
    p->conn.send(wire::encode_eval_request(req));

    if (fut.wait_for(std::chrono::milliseconds(opts.timeout_ms)) != std::future_status::ready) {
        std::lock_guard<std::mutex> lk(p->pending_mu);
        p->pending.erase(id);
        fail.error_message = "timeout (" + std::to_string(opts.timeout_ms) + " ms)";
        return fail;
    }
    return fut.get();
}

void Session::eval_async(const std::string& code,
                          const EvalOptions& opts,
                          std::function<void(EvalResult)> cb) {
    if (!p || !p->conn.is_open()) {
        EvalResult r; r.ok = false; r.error_message = "not connected";
        if (cb) cb(std::move(r));
        return;
    }

    uint64_t id = p->next_id.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(p->pending_async_mu);
        p->pending_async.emplace(id, std::move(cb));
    }

    wire::EvalRequest req;
    req.id   = id;
    req.code = code;
    req.world = world_to_wire(opts.world);
    if (opts.world == World::Renderer) req.world_index = opts.window_index;
    p->conn.send(wire::encode_eval_request(req));
}

uint64_t Session::install_hook(const std::string& symbol) {
    if (!p || !p->conn.is_open()) return 0;
    uint64_t id = p->next_id.fetch_add(1, std::memory_order_relaxed);
    wire::HookInstallRequest req{ id, symbol, "" };
    p->conn.send(wire::encode_hook_install(req));
    return id;
}

void Session::on_hook_hit(HookHitHandler handler) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(p->hook_mu);
    p->hook_handler = std::move(handler);
}

} // namespace positron::sdk
