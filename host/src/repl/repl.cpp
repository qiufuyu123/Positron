#include "repl.h"
#include "pipe_client/pipe_client.h"
#include <wire/wire.h>
#include <replxx.hxx>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using positron::wire::Json;

namespace positron::repl {

static std::atomic<uint64_t> g_id_counter{1};

static void print_event(replxx::Replxx& rx, const Json& msg) {
    auto kind = msg.value("kind", std::string{});
    if (kind == "log") {
        rx.print(("[log] " + msg.value("msg", std::string{}) + "\n").c_str());
    } else if (kind == "hook.hit") {
        rx.print(("[hook.hit] " + msg.dump() + "\n").c_str());
    } else if (kind == "ready") {
        rx.print(("[ready] " + msg.dump() + "\n").c_str());
    } else {
        rx.print(("[event] " + msg.dump() + "\n").c_str());
    }
}

static void print_help(replxx::Replxx& rx) {
    rx.print(
        "Commands:\n"
        "  <expression>          eval JS in the current world (see .world)\n"
        "  .world auto|node      eval directly in target's main V8 context (default)\n"
        "  .world renderer[:N]   eval in BrowserWindow[N]'s renderer main world\n"
        "                        via webContents.executeJavaScript\n"
        "  .hook <symbol>        install a runtime hook on a target export\n"
        "  .detach / .quit       disable hooks, close, exit REPL\n"
        "  .help                 this message\n");
}

int run(positron::pipe::Client& c, uint32_t target_pid) {
    replxx::Replxx rx;
    rx.set_max_history_size(1000);
    rx.set_word_break_characters(" \t.,()[]{}'\"");

    // Per-session world state. Toggled by `.world ...`.
    std::string world = "auto";
    int window_index  = 0;

    auto build_prompt = [&]{
        std::string p = "positron[" + std::to_string(target_pid);
        if (world == "renderer") {
            p += "/r" + std::to_string(window_index);
        }
        return p + "]> ";
    };

    rx.print("Type .help for commands, .detach to quit.\n");

    while (true) {
        // Drain pending events without blocking, so async output (hook hits,
        // logs) appears between prompts.
        while (auto m = c.recv(0)) {
            if (m->value("kind", std::string{}) != "eval.response") {
                print_event(rx, *m);
            }
        }
        if (!c.is_open()) {
            rx.print("\n[connection closed]\n");
            break;
        }

        const char* line_c = rx.input(build_prompt());
        if (!line_c) break;
        std::string line = line_c;
        if (line.empty()) continue;
        rx.history_add(line);

        if (line == ".help") { print_help(rx); continue; }
        if (line == ".quit" || line == ".detach") {
            wire::Json detach{{"kind", "detach"}};
            c.send(detach);
            break;
        }
        if (line.rfind(".hook ", 0) == 0) {
            wire::HookInstallRequest req{ g_id_counter.fetch_add(1), line.substr(6), "" };
            c.send(wire::encode_hook_install(req));
            rx.print(("[hook.install queued: " + req.symbol + "]\n").c_str());
            continue;
        }
        if (line.rfind(".world", 0) == 0) {
            // `.world auto`, `.world node`, `.world renderer`, `.world renderer:2`
            std::string arg = line.size() > 6 ? line.substr(7) : "";
            if (arg.empty()) {
                rx.print(("current world: " + world +
                          (world == "renderer" ? (":" + std::to_string(window_index)) : "") +
                          "\n").c_str());
                continue;
            }
            std::string base = arg;
            int idx = 0;
            auto colon = arg.find(':');
            if (colon != std::string::npos) {
                base = arg.substr(0, colon);
                try { idx = std::stoi(arg.substr(colon + 1)); } catch (...) { idx = 0; }
            }
            if (base == "auto" || base == "node" || base == "renderer") {
                world = base;
                window_index = idx;
                rx.print(("[world = " + world +
                          (world == "renderer" ? (":" + std::to_string(window_index)) : "") +
                          "]\n").c_str());
            } else {
                rx.print(("unknown world '" + arg + "' (use auto / node / renderer[:N])\n").c_str());
            }
            continue;
        }

        // Default: eval in current world
        wire::EvalRequest req;
        req.id = g_id_counter.fetch_add(1);
        req.code = line;
        req.world = world;
        if (world == "renderer") req.world_index = window_index;
        c.send(wire::encode_eval_request(req));

        bool got = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            auto m = c.recv(500);
            if (!m) {
                if (!c.is_open()) break;
                continue;
            }
            if (m->value("kind", std::string{}) != "eval.response") {
                print_event(rx, *m);
                continue;
            }
            if (m->value("id", uint64_t{0}) != req.id) continue;
            auto er = wire::decode_eval_response(*m);
            if (er.ok && er.result) {
                rx.print((er.result->json + "\n").c_str());
            } else if (er.error) {
                std::string s = "error: " + er.error->message + "\n";
                if (!er.error->stack.empty()) s += er.error->stack + "\n";
                rx.print(s.c_str());
            }
            got = true;
            break;
        }
        if (!got) rx.print("[timeout waiting for eval response]\n");
    }
    return 0;
}

} // namespace positron::repl
