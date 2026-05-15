#include "repl/repl.h"

#include <positron/sdk.h>

#include <CLI/CLI.hpp>
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

int do_list() {
    auto procs = positron::sdk::list_processes();
    if (procs.empty()) {
        std::cerr << "no electron processes found\n";
        return 0;
    }
    std::wcout << L"PID\tTYPE\tEXE\n";
    for (auto& p : procs) {
        std::wcout << p.pid << L"\t"
                   << positron::sdk::type_name(p.type) << L"\t"
                   << p.exe_name << L"\n";
    }
    return 0;
}

struct AttachArgs {
    uint32_t                pid = 0;
    std::wstring            dll;       // empty -> SDK default (next to host.exe)
    positron::sdk::Transport transport = positron::sdk::Transport::V2;
};

const char* mode_label(positron::sdk::Transport t) {
    return t == positron::sdk::Transport::V2 ? "v2" : "native";
}

void print_unmap_status(positron::sdk::Session& s) {
    if (s.transport() != positron::sdk::Transport::V2) return;
    auto base = s.injected_module_base();
    if (base == 0) {
        std::cout << "[v2 reattached to existing JS server, no inject]\n";
        return;
    }
    // The payload schedules its self-unmap with a ~500ms delay so all
    // detached threads can drain. Give it a beat, then probe.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    std::cout << "[v2 self-unmap " << (s.verify_payload_unmapped() ? "ok" : "PENDING")
              << " base=0x" << std::hex << base << std::dec << "]\n";
}

int do_eval(const AttachArgs& a, const std::string& code, const positron::sdk::EvalOptions& opts) {
    positron::sdk::Session s;
    if (auto err = s.attach(a.pid, a.dll, a.transport)) {
        std::cerr << "inject FAIL: " << err->message << "\n";
        return 2;
    }
    std::cout << "[connected pid=" << a.pid
              << " transport=" << mode_label(s.transport()) << "]\n";
    print_unmap_status(s);

    auto t0 = std::chrono::steady_clock::now();
    auto r  = s.eval(code, opts);
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    int rc = 0;
    if (r.ok) {
        std::cout << r.json_value << "\n";
    } else {
        std::cerr << "error: " << r.error_message << "\n";
        if (!r.error_stack.empty()) std::cerr << r.error_stack << "\n";
        rc = 6;
    }
    s.detach();
    (void)ms;
    return rc;
}

int do_run_script(const AttachArgs& a, const std::string& script_path,
                  const positron::sdk::EvalOptions& opts) {
    std::ifstream f(script_path);
    if (!f) { std::cerr << "cannot open script: " << script_path << "\n"; return 7; }
    std::stringstream ss; ss << f.rdbuf();
    return do_eval(a, ss.str(), opts);
}

int do_attach_repl(const AttachArgs& a) {
    positron::sdk::Session s;
    if (auto err = s.attach(a.pid, a.dll, a.transport)) {
        std::cerr << "inject FAIL: " << err->message << "\n";
        return 2;
    }
    std::cout << "[connected pid=" << a.pid
              << " transport=" << mode_label(s.transport()) << "]\n";
    print_unmap_status(s);
    int rc = positron::repl::run(s, a.pid);
    s.detach();
    return rc;
}

} // anonymous

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> argv_storage;
    argv_storage.reserve(argc);
    for (int i = 0; i < argc; ++i) argv_storage.push_back(narrow(argv[i]));
    std::vector<const char*> argv_utf8;
    argv_utf8.reserve(argc);
    for (auto& s : argv_storage) argv_utf8.push_back(s.c_str());

    CLI::App app{"positron — Electron process inspector / JS injector"};
    app.require_subcommand(1);

    auto* list = app.add_subcommand("list", "list candidate Electron processes");

    auto* attach = app.add_subcommand("attach", "attach to a process and start an interactive REPL");
    uint32_t a_pid = 0;
    std::string a_dll;
    bool a_native = false;
    attach->add_option("pid", a_pid, "target process ID")->required();
    attach->add_option("--dll", a_dll, "path to payload.dll (default: next to host.exe)");
    attach->add_flag("--native", a_native,
                     "use v1 (native) transport — required for .hook on native exports; default is v2 (JS)");

    auto* run_cmd = app.add_subcommand("run", "attach, execute a JS file, exit");
    uint32_t r_pid = 0;
    int r_rwin = 0;
    std::string r_dll, r_script;
    bool r_renderer = false;
    bool r_native = false;
    run_cmd->add_option("pid", r_pid, "target process ID")->required();
    run_cmd->add_option("script", r_script, "path to .js file")->required();
    run_cmd->add_option("--dll", r_dll, "path to payload.dll");
    run_cmd->add_flag("--renderer,-r", r_renderer,
                      "hop into the BrowserWindow's renderer main world via webContents.executeJavaScript");
    run_cmd->add_option("--window", r_rwin, "BrowserWindow index when --renderer is set");
    run_cmd->add_flag("--native", r_native, "use v1 (native) transport instead of v2");

    auto* eval = app.add_subcommand("eval", "attach, evaluate one expression, exit");
    uint32_t e_pid = 0;
    int e_rwin = 0;
    std::string e_dll, e_expr;
    bool e_renderer = false;
    bool e_native = false;
    eval->add_option("pid", e_pid, "target process ID")->required();
    eval->add_option("expr", e_expr, "JS expression")->required();
    eval->add_option("--dll", e_dll, "path to payload.dll");
    eval->add_flag("--renderer,-r", e_renderer,
                   "hop into the BrowserWindow's renderer main world via webContents.executeJavaScript");
    eval->add_option("--window", e_rwin, "BrowserWindow index when --renderer is set");
    eval->add_flag("--native", e_native, "use v1 (native) transport instead of v2");

    try { app.parse(argc, argv_utf8.data()); }
    catch (const CLI::ParseError& e) { return app.exit(e); }

    if (*list) return do_list();

    auto pick_transport = [](bool native) {
        return native ? positron::sdk::Transport::Native : positron::sdk::Transport::V2;
    };

    if (*attach) {
        AttachArgs a{ a_pid, a_dll.empty() ? std::wstring{} : widen(a_dll), pick_transport(a_native) };
        return do_attach_repl(a);
    }
    if (*run_cmd) {
        AttachArgs a{ r_pid, r_dll.empty() ? std::wstring{} : widen(r_dll), pick_transport(r_native) };
        positron::sdk::EvalOptions opts;
        if (r_renderer) {
            opts.world = positron::sdk::World::Renderer;
            opts.window_index = r_rwin;
        }
        return do_run_script(a, r_script, opts);
    }
    if (*eval) {
        AttachArgs a{ e_pid, e_dll.empty() ? std::wstring{} : widen(e_dll), pick_transport(e_native) };
        positron::sdk::EvalOptions opts;
        if (e_renderer) {
            opts.world = positron::sdk::World::Renderer;
            opts.window_index = e_rwin;
        }
        return do_eval(a, e_expr, opts);
    }
    return 0;
}
