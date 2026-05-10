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
        "  <expression>     evaluate JS in the target's V8 context\n"
        "  .hook <symbol>   install a runtime hook on a target export\n"
        "  .detach          disable hooks, close the connection, exit REPL\n"
        "  .quit            same as .detach\n"
        "  .help            this message\n"
        "Anything else is treated as a JS expression.\n");
}

int run(positron::pipe::Client& c, uint32_t target_pid) {
    replxx::Replxx rx;
    rx.set_max_history_size(1000);
    rx.set_word_break_characters(" \t.,()[]{}'\"");
    std::string prompt = "positron[" + std::to_string(target_pid) + "]> ";

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

        const char* line_c = rx.input(prompt);
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

        // Default: eval
        wire::EvalRequest req;
        req.id = g_id_counter.fetch_add(1);
        req.code = line;
        req.world = "auto";
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
