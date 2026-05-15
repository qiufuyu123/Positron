#include "repl.h"

#include <positron/sdk.h>

#include <replxx.hxx>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace positron::repl {

namespace {

void print_help(replxx::Replxx& rx) {
    rx.print(
        "Commands:\n"
        "  <expression>          eval JS in the current world (see .world)\n"
        "  .world auto|node      eval directly in target's main V8 context (default)\n"
        "  .world renderer[:N]   eval in BrowserWindow[N]'s renderer main world\n"
        "                        via webContents.executeJavaScript\n"
        "  .hook <symbol>        install a runtime hook on a target export\n"
        "  .dump [name]          (renderer world) save current page HTML to\n"
        "                        <name>.html in cwd; default name is a timestamp\n"
        "  .mod load <file>      load a JS module into the target process\n"
        "  .mod unload <name>    unload a previously loaded module by name\n"
        "  .mod list             list currently loaded modules\n"
        "  .shutdown             close TCP server (modules keep running)\n"
        "  .detach / .quit       disable hooks, close, exit REPL\n"
        "  .help                 this message\n");
}

std::vector<uint8_t> base64_decode(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t buf = 0;
    int      bits = 0;
    for (char c : s) {
        int v;
        if      (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+')             v = 62;
        else if (c == '/')             v = 63;
        else                           continue; // skip '=', whitespace, anything else
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::string default_dump_name() {
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "dump_%04d%02d%02d_%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string trim(std::string v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
    while (!v.empty() && (v.back()  == ' ' || v.back()  == '\t')) v.pop_back();
    return v;
}

} // anonymous

int run(positron::sdk::Session& s, uint32_t target_pid) {
    replxx::Replxx rx;
    rx.set_max_history_size(1000);
    rx.set_word_break_characters(" \t.,()[]{}'\"");

    // World state
    positron::sdk::World current_world = positron::sdk::World::Auto;
    int                 window_index   = 0;
    auto world_label = [&]() -> std::string {
        if (current_world == positron::sdk::World::Renderer)
            return "/r" + std::to_string(window_index);
        return "";
    };
    auto build_prompt = [&]{
        return "positron[" + std::to_string(target_pid) + world_label() + "]> ";
    };

    // Stream incoming hook hits via a thread-safe queue printed before each
    // prompt. We can't print directly from the SDK callback because replxx's
    // input loop owns stdout while idle.
    std::mutex                hit_mu;
    std::vector<std::string>  hit_queue;
    s.on_hook_hit([&](const positron::sdk::HookHit& h){
        std::ostringstream os;
        os << "[hook.hit id=" << h.hook_id << " args=[";
        for (size_t i = 0; i < h.args.size(); ++i) {
            if (i) os << ",";
            os << "0x" << std::hex << h.args[i];
        }
        os << "]";
        if (h.dropped_since_last)
            os << " dropped=" << std::dec << h.dropped_since_last;
        os << "]";
        std::lock_guard<std::mutex> lk(hit_mu);
        hit_queue.push_back(os.str());
    });

    // Unsolicited messages from bootstrap (log, mod.event, …)
    std::mutex                msg_mu;
    std::vector<std::string>  msg_queue;
    s.on_message([&](const positron::sdk::Message& m){
        // Light-weight: just prefix with the kind and dump the raw JSON.
        // For log frames the JSON already contains "level" and "message";
        // for mod.event it has "module" and "data".
        std::string line = "[" + m.kind + "] " + m.json;
        std::lock_guard<std::mutex> lk(msg_mu);
        msg_queue.push_back(std::move(line));
    });

    rx.print("Type .help for commands, .detach to quit.\n");

    while (true) {
        // Drain any queued hook events + messages between prompts
        {
            std::lock_guard<std::mutex> lk(hit_mu);
            for (auto& line : hit_queue) rx.print((line + "\n").c_str());
            hit_queue.clear();
        }
        {
            std::lock_guard<std::mutex> lk(msg_mu);
            for (auto& line : msg_queue) rx.print((line + "\n").c_str());
            msg_queue.clear();
        }
        if (!s.is_connected()) {
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
            break;
        }
        if (line == ".shutdown") {
            auto r = s.shutdown_server();
            if (r.ok) {
                rx.print("[shutdown] TCP server closed. modules still running.\n");
            } else {
                rx.print(("[shutdown error] " + r.error_message + "\n").c_str());
            }
            break;
        }
        if (line.rfind(".hook ", 0) == 0) {
            std::string sym = line.substr(6);
            uint64_t id = s.install_hook(sym);
            if (id) {
                rx.print(("[hook.install queued: " + sym + " (id=" + std::to_string(id) + ")]\n").c_str());
            } else {
                rx.print("[hook.install failed: not connected]\n");
            }
            continue;
        }
        if (line == ".dump" || line.rfind(".dump ", 0) == 0) {
            if (current_world != positron::sdk::World::Renderer) {
                rx.print("[.dump] only available in renderer world (use .world renderer[:N])\n");
                continue;
            }
            std::string name = trim(line.size() > 5 ? line.substr(6) : std::string{});
            if (name.empty()) name = default_dump_name();
            if (name.size() < 5 || name.substr(name.size() - 5) != ".html") name += ".html";

            // Renderer-side: emit doctype + outerHTML, UTF-8 -> base64 to dodge
            // any escape headaches over the JSON wire.
            const std::string js =
                "(function(){"
                "var d=document.doctype;"
                "var dt=d?('<!DOCTYPE '+d.name"
                "+(d.publicId?(' PUBLIC \"'+d.publicId+'\"'):'')"
                "+(d.systemId?(' \"'+d.systemId+'\"'):'')"
                "+'>\\n'):'';"
                "var html=dt+document.documentElement.outerHTML;"
                "var b=new TextEncoder().encode(html);"
                "var s='';for(var i=0;i<b.length;i++)s+=String.fromCharCode(b[i]);"
                "return btoa(s);"
                "})()";

            positron::sdk::EvalOptions opts;
            opts.world        = current_world;
            opts.window_index = window_index;
            opts.timeout_ms   = 60000;
            auto r = s.eval(js, opts);
            if (!r.ok) {
                rx.print(("[.dump failed] " + r.error_message + "\n").c_str());
                continue;
            }
            // r.json_value should be a JSON string of pure base64 chars: "AbCdEf="
            const std::string& jv = r.json_value;
            if (jv.size() < 2 || jv.front() != '"' || jv.back() != '"') {
                rx.print("[.dump failed] unexpected eval result\n");
                continue;
            }
            std::string b64   = jv.substr(1, jv.size() - 2);
            auto        bytes = base64_decode(b64);

            std::ofstream ofs(name, std::ios::binary);
            if (!ofs) {
                rx.print(("[.dump failed] cannot open " + name + " for writing\n").c_str());
                continue;
            }
            if (!bytes.empty())
                ofs.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
            ofs.close();

            rx.print(("[dumped " + std::to_string(bytes.size()) +
                      " bytes -> " + name + "]\n").c_str());
            continue;
        }
        if (line.rfind(".mod ", 0) == 0 || line == ".mod") {
            std::string sub = trim(line.size() > 4 ? line.substr(5) : std::string{});
            if (sub.rfind("load ", 0) == 0) {
                std::string path = trim(sub.substr(5));
                if (path.empty()) { rx.print("[.mod load] usage: .mod load <file.js>\n"); continue; }
                std::ifstream mf(path, std::ios::binary);
                if (!mf) { rx.print(("[.mod load] cannot open: " + path + "\n").c_str()); continue; }
                std::stringstream ss; ss << mf.rdbuf();
                std::string code = ss.str();
                if (code.empty()) { rx.print("[.mod load] file is empty\n"); continue; }

                // Escape for JS template literal
                std::string escaped;
                for (char c : code) {
                    if (c == '\\') escaped += "\\\\";
                    else if (c == '`') escaped += "\\`";
                    else if (c == '$') escaped += "\\$";
                    else escaped += c;
                }
                std::string js =
                    "(function(){for(var k in globalThis){if(k[0]==='_'&&globalThis[k]&&globalThis[k].i)return globalThis[k].i}return null})().loadModule(`" + escaped + "`)";

                positron::sdk::EvalOptions opts;
                opts.timeout_ms = 10000;
                auto r = s.eval(js, opts);
                if (r.ok) {
                    rx.print(("[mod.load] " + r.json_value + "\n").c_str());
                } else {
                    rx.print(("[mod.load error] " + r.error_message + "\n").c_str());
                }
                continue;
            }
            if (sub.rfind("unload ", 0) == 0) {
                std::string modname = trim(sub.substr(7));
                if (modname.empty()) { rx.print("[.mod unload] usage: .mod unload <name>\n"); continue; }
                std::string js =
                    "(function(){for(var k in globalThis){if(k[0]==='_'&&globalThis[k]&&globalThis[k].i)return globalThis[k].i}return null})().unloadModule('" + modname + "')";
                positron::sdk::EvalOptions opts;
                opts.timeout_ms = 5000;
                auto r = s.eval(js, opts);
                if (r.ok) {
                    rx.print(("[mod.unload] " + r.json_value + "\n").c_str());
                } else {
                    rx.print(("[mod.unload error] " + r.error_message + "\n").c_str());
                }
                continue;
            }
            if (sub == "list" || sub.empty()) {
                std::string js =
                    "(function(){for(var k in globalThis){if(k[0]==='_'&&globalThis[k]&&globalThis[k].i){var _s=globalThis[k].i;"
                    "return _s.modules?Object.keys(_s.modules):[]}}return[]})()";
                positron::sdk::EvalOptions opts;
                opts.timeout_ms = 3000;
                auto r = s.eval(js, opts);
                if (r.ok) {
                    rx.print(("[modules] " + r.json_value + "\n").c_str());
                } else {
                    rx.print(("[mod.list error] " + r.error_message + "\n").c_str());
                }
                continue;
            }
            rx.print("[.mod] unknown subcommand. try: load <file>, unload <name>, list\n");
            continue;
        }
        if (line.rfind(".world", 0) == 0) {
            std::string arg = line.size() > 6 ? line.substr(7) : "";
            if (arg.empty()) {
                rx.print(("current world: " +
                          std::string{current_world == positron::sdk::World::Renderer ? "renderer" :
                                      current_world == positron::sdk::World::Node     ? "node" : "auto"} +
                          (current_world == positron::sdk::World::Renderer ? ":" + std::to_string(window_index) : std::string{}) +
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
            if (base == "auto")          { current_world = positron::sdk::World::Auto;     window_index = 0; }
            else if (base == "node")     { current_world = positron::sdk::World::Node;     window_index = 0; }
            else if (base == "renderer") { current_world = positron::sdk::World::Renderer; window_index = idx; }
            else {
                rx.print(("unknown world '" + arg + "' (use auto / node / renderer[:N])\n").c_str());
                continue;
            }
            rx.print(("[world = " + base +
                      (base == "renderer" ? ":" + std::to_string(window_index) : std::string{}) +
                      "]\n").c_str());
            continue;
        }

        // Default: eval in current world
        positron::sdk::EvalOptions opts;
        opts.world        = current_world;
        opts.window_index = window_index;
        opts.timeout_ms   = 30000;
        auto r = s.eval(line, opts);
        if (r.ok) {
            rx.print((r.json_value + "\n").c_str());
        } else {
            std::string msg = "error: " + r.error_message + "\n";
            if (!r.error_stack.empty()) msg += r.error_stack + "\n";
            rx.print(msg.c_str());
        }
    }
    return 0;
}

} // namespace positron::repl
