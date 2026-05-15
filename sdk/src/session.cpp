#include <positron/session.h>

#include "injector/injector.h"
#include "pipe_client/pipe_client.h"
#include "bootstrap_v2.h"
#include <wire/wire.h>

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "Ws2_32.lib")

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
    std::unique_ptr<pipe::Client> conn = std::make_unique<pipe::Client>();
    std::thread           reader;
    std::atomic<bool>     reader_running{false};
    std::atomic<uint64_t> next_id{1};
    Transport             active_transport = Transport::Native;
    uint32_t              target_pid       = 0;
    uint64_t              injected_base    = 0;

    std::mutex                                            pending_mu;
    std::unordered_map<uint64_t, std::promise<EvalResult>> pending;

    std::mutex                pending_async_mu;
    std::unordered_map<uint64_t, std::function<void(EvalResult)>> pending_async;

    std::mutex      hook_mu;
    HookHitHandler  hook_handler;

    std::mutex       msg_mu;
    MessageHandler   msg_handler;

    ~Impl() { stop(); }

    void start_reader() {
        reader_running = true;
        reader = std::thread([this]{ this->run_reader(); });
    }

    void stop() {
        reader_running = false;
        conn->close();
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
            auto msg = conn->recv(200);
            if (!msg) {
                if (!conn->is_open()) break;
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
            } else {
                // Forward unsolicited frames (log, mod.event, etc.) to the
                // message handler so the REPL / consumer can display them.
                MessageHandler mcb;
                {
                    std::lock_guard<std::mutex> lk(msg_mu);
                    mcb = msg_handler;
                }
                if (mcb) {
                    Message m;
                    m.kind = kind;
                    try { m.json = msg->dump(); } catch (...) {}
                    try { mcb(m); } catch (...) {}
                }
            }
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
Session::attach(uint32_t pid, const std::wstring& payload_dll, Transport transport) {
    if (!p) p = std::make_unique<Impl>();
    p->target_pid = pid;

    // V2 fast path: if a previous attach already left a JS server live in
    // the target, just connect to it. Avoids re-injecting (which would
    // accumulate stale BlackBone VEH handlers in the target's exception
    // chain, eventually crashing it).
    if (transport == Transport::V2) {
        const uint16_t v2_port = static_cast<uint16_t>(60000u + (pid % 5000u));
        auto probe = std::make_unique<pipe::Client>();
        auto pr = probe->connect_to(v2_port, 500);
        if (std::holds_alternative<positron::pipe::Connected>(pr)) {
            // Server is up. Drain the JS-side hello and adopt the connection.
            auto h = probe->recv(2000);
            if (h) {
                p->conn = std::move(probe);
                p->active_transport = Transport::V2;
                p->injected_base    = 0;     // no inject this session
                p->start_reader();
                return std::nullopt;
            }
            probe->close();
        }
    }

    std::wstring dll = payload_dll.empty() ? default_payload_path() : payload_dll;

    auto rr = positron::injector::inject({pid, dll, 10000});
    if (auto* e = std::get_if<positron::injector::Error>(&rr)) {
        return AttachError{e->message};
    }
    auto& succ = std::get<positron::injector::Success>(rr);
    p->injected_base = succ.module_base;

    auto cr = p->conn->connect(pid, 5000);
    if (auto* e = std::get_if<positron::pipe::ConnectError>(&cr)) {
        return AttachError{e->message};
    }

    // Drain the v1 HELLO frame.
    auto hello = p->conn->recv(5000);
    if (!hello) return AttachError{"no HELLO frame received from payload"};

    // Tell the payload which transport mode to use.
    p->conn->send(wire::Json{{"kind", "transport.select"},
                            {"mode", transport == Transport::V2 ? "v2" : "native"}});

    // Send VEH + scratch page addresses so the payload can clean up
    // BlackBone's residue during phase-2 self-unmap.
    p->conn->send(wire::Json{
        {"kind",           "teardown.info"},
        {"veh_handle",     succ.veh_handle},
        {"veh_code_addr",  succ.veh_code_addr},
        {"veh_code_size",  succ.veh_code_size},
        {"mod_table_addr", succ.mod_table_addr},
        {"mod_table_size", succ.mod_table_size},
    });

    if (transport == Transport::Native) {
        p->active_transport = Transport::Native;
        p->start_reader();
        return std::nullopt;
    }

    // ---- v2 negotiation ---------------------------------------------------
    // Ship the bootstrap JS to the payload and wait for the JS server to
    // signal readiness. On any failure the SDK aborts the attach so the
    // caller can decide whether to retry with Transport::Native.
    auto js = positron::sdk::bootstrap::load_bootstrap_v2_js();
    if (!js) {
        p->conn->close();
        return AttachError{
            "bootstrap.js not found. Place it next to host.exe (the SDK build "
            "should copy it to the output directory) or set "
            "POSITRON_BOOTSTRAP_JS_PATH=<absolute path>. Alternatively run "
            "with Transport::Native."};
    }
    p->conn->send(wire::Json{{"kind", "bootstrap.start"},
                            {"code", js->source}});

    uint16_t v2_port = 0;
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline) {
            auto msg = p->conn->recv(500);
            if (!msg) {
                if (!p->conn->is_open()) return AttachError{"v1 socket closed during v2 bootstrap"};
                continue;
            }
            auto kind = msg->value("kind", std::string{});
            if (kind == "v2.ready") {
                v2_port = static_cast<uint16_t>(msg->value("port", 0));
                break;
            }
            if (kind == "v2.error") {
                std::string m = msg->value("message", std::string{"unknown"});
                p->conn->close();
                return AttachError{"v2 bootstrap failed: " + m + " — retry with Transport::Native"};
            }
            // Other frames (logs, stray events) ignored during negotiation.
        }
    }
    if (v2_port == 0) {
        p->conn->close();
        return AttachError{"v2 bootstrap timeout (no v2.ready) — retry with Transport::Native"};
    }

    // Open the v2 channel to the JS server.
    auto v2 = std::make_unique<pipe::Client>();
    auto v2cr = v2->connect_to(v2_port, 5000);
    if (auto* e = std::get_if<positron::pipe::ConnectError>(&v2cr)) {
        p->conn->close();
        return AttachError{"v2 connect_to(" + std::to_string(v2_port) + ") failed: " + e->message};
    }
    auto v2_hello = v2->recv(5000);
    if (!v2_hello) {
        v2->close();
        p->conn->close();
        return AttachError{"no HELLO from v2 JS server"};
    }

    // Tell the payload host has switched, then close v1.
    p->conn->send(wire::Json{{"kind", "v2.commit"}});
    // Tiny grace so the commit frame leaves before the v1 socket FIN.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    p->conn->close();

    // Swap pipe::Client unique_ptrs — drops the now-defunct v1 client and
    // adopts the v2 one as the session's transport.
    p->conn = std::move(v2);
    p->active_transport = Transport::V2;
    p->start_reader();
    return std::nullopt;
}

Transport Session::transport() const {
    return p ? p->active_transport : Transport::Native;
}

uint64_t Session::injected_module_base() const {
    return p ? p->injected_base : 0;
}

bool Session::verify_payload_unmapped() {
    if (!p || p->injected_base == 0 || p->target_pid == 0) return false;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                             FALSE, p->target_pid);
    if (!h) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    SIZE_T n = ::VirtualQueryEx(h,
        reinterpret_cast<LPCVOID>(p->injected_base),
        &mbi, sizeof(mbi));
    ::CloseHandle(h);
    if (n != sizeof(mbi)) return false;
    return mbi.State == MEM_FREE;
}

void Session::detach() {
    if (!p) return;
    if (p->conn->is_open()) {
        wire::Json detach{{"kind", "detach"}};
        p->conn->send(detach);
    }
    p->stop();
}

bool Session::is_connected() const {
    return p && p->conn->is_open();
}

EvalResult Session::eval(const std::string& code, const EvalOptions& opts) {
    EvalResult fail;
    fail.ok = false;

    if (!p || !p->conn->is_open()) {
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
    p->conn->send(wire::encode_eval_request(req));

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
    if (!p || !p->conn->is_open()) {
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
    p->conn->send(wire::encode_eval_request(req));
}

uint64_t Session::install_hook(const std::string& symbol) {
    if (!p || !p->conn->is_open()) return 0;
    if (p->active_transport == Transport::V2) {
        // Native function detours need C++-side machine-code patching; the
        // v2 transport runs in JS only. Caller should reattach with
        // Transport::Native if they need .hook semantics.
        return 0;
    }
    uint64_t id = p->next_id.fetch_add(1, std::memory_order_relaxed);
    wire::HookInstallRequest req{ id, symbol, "" };
    p->conn->send(wire::encode_hook_install(req));
    return id;
}

void Session::on_hook_hit(HookHitHandler handler) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(p->hook_mu);
    p->hook_handler = std::move(handler);
}

void Session::on_message(MessageHandler handler) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(p->msg_mu);
    p->msg_handler = std::move(handler);
}

// ---- Module system --------------------------------------------------------

namespace {

std::string escape_for_template_literal(const std::string& code) {
    std::string out;
    out.reserve(code.size() + 256);
    for (char c : code) {
        if (c == '\\')     out += "\\\\";
        else if (c == '`') out += "\\`";
        else if (c == '$') out += "\\$";
        else               out += c;
    }
    return out;
}

std::string read_file_utf8(const std::wstring& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

} // anonymous

EvalResult Session::load_module(const std::string& js_code, uint32_t timeout_ms) {
    std::string js = "(function(){for(var k in globalThis){if(k[0]==='_'&&globalThis[k]&&globalThis[k].i)return globalThis[k].i}return null})().loadModule(`"
                   + escape_for_template_literal(js_code) + "`)";
    return eval(js, {World::Auto, 0, timeout_ms});
}

EvalResult Session::load_module_file(const std::wstring& path, uint32_t timeout_ms) {
    // Convert wstring path to UTF-8 for JS, escape backslashes for string literal
    std::string utf8;
    {
        int len = ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            utf8.resize(len);
            ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(), utf8.data(), len, nullptr, nullptr);
        }
    }
    if (utf8.empty()) {
        EvalResult r;
        r.ok = false;
        r.error_message = "invalid file path";
        return r;
    }
    // Escape backslashes and quotes for JS string literal
    std::string escaped;
    escaped.reserve(utf8.size() + 32);
    for (char c : utf8) {
        if (c == '\\') escaped += "\\\\";
        else if (c == '\'') escaped += "\\'";
        else escaped += c;
    }
    std::string js = "(function(){for(var k in globalThis){if(k[0]==='_'&&globalThis[k]&&globalThis[k].i)return globalThis[k].i}return null})().loadModuleFromFile('" + escaped + "')";
    return eval(js, {World::Auto, 0, timeout_ms});
}

EvalResult Session::unload_module(const std::string& name, uint32_t timeout_ms) {
    std::string js = "(function(){for(var k in globalThis){if(k[0]==='_'&&globalThis[k]&&globalThis[k].i)return globalThis[k].i}return null})().unloadModule('" + name + "')";
    return eval(js, {World::Auto, 0, timeout_ms});
}

EvalResult Session::list_modules(uint32_t timeout_ms) {
    return eval("(function(){for(var k in globalThis){if(k[0]==='_'&&globalThis[k]&&globalThis[k].i)return globalThis[k].i}return null})().listModules()", {World::Auto, 0, timeout_ms});
}

EvalResult Session::shutdown_server(uint32_t timeout_ms) {
    return eval("(function(){for(var k in globalThis){if(k[0]==='_'&&globalThis[k]&&globalThis[k].i)return globalThis[k].i}return null})().shutdown()", {World::Auto, 0, timeout_ms});
}

} // namespace positron::sdk
