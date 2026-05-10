#include "injector/injector.h"
#include "pipe_client/pipe_client.h"
#include "process_enum/process_enum.h"
#include "repl/repl.h"
#include <wire/wire.h>
#include <CLI11.hpp>
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using positron::wire::Json;

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n - 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Resolve payload.dll. Defaults to <host.exe>\..\payload.dll. Allow override
// via --dll <path>.
std::wstring default_payload_path() {
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (std::filesystem::path{buf}.parent_path() / L"payload.dll").wstring();
}

int do_list() {
    auto entries = positron::proc::enumerate_electron();
    if (entries.empty()) {
        std::cerr << "no electron processes found\n";
        return 0;
    }
    std::wcout << L"PID\tTYPE\tEXE\n";
    for (auto& e : entries) {
        std::wcout << e.pid << L"\t"
                   << positron::proc::type_name(e.type) << L"\t"
                   << e.exe_name << L"\n";
    }
    return 0;
}

struct AttachContext {
    uint32_t pid;
    std::wstring dll;
};

int do_attach_setup(AttachContext& ctx, positron::pipe::Client& client) {
    auto inject_result = positron::injector::inject({ctx.pid, ctx.dll, 10000});
    if (auto* e = std::get_if<positron::injector::Error>(&inject_result)) {
        std::cerr << "inject FAIL: " << e->message << "\n";
        return 2;
    }
    std::cout << "[inject ok]\n";

    auto cr = client.connect(ctx.pid, 5000);
    if (auto* e = std::get_if<positron::pipe::ConnectError>(&cr)) {
        std::cerr << "connect FAIL: " << e->message << "\n";
        return 3;
    }
    auto hello = client.recv(5000);
    if (!hello) {
        std::cerr << "no HELLO frame received\n";
        return 4;
    }
    std::cout << "[connected pid=" << ctx.pid << "]\n";
    return 0;
}

int do_eval(AttachContext& ctx, const std::string& code, uint32_t timeout_ms) {
    positron::pipe::Client c;
    if (int rc = do_attach_setup(ctx, c); rc != 0) return rc;

    positron::wire::EvalRequest req{1, code, "auto", std::nullopt};
    auto t0 = std::chrono::steady_clock::now();
    c.send(positron::wire::encode_eval_request(req));

    auto resp = c.recv(timeout_ms);
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (!resp) {
        std::cerr << "no eval response (waited " << ms << "ms)\n";
        return 5;
    }
    auto er = positron::wire::decode_eval_response(*resp);
    if (er.ok && er.result) {
        std::cout << er.result->json << "\n";
        return 0;
    }
    if (er.error) {
        std::cerr << "error: " << er.error->message << "\n";
        if (!er.error->stack.empty()) std::cerr << er.error->stack << "\n";
    }
    return 6;
}

int do_attach_repl(AttachContext& ctx) {
    positron::pipe::Client c;
    if (int rc = do_attach_setup(ctx, c); rc != 0) return rc;
    return positron::repl::run(c, ctx.pid);
}

int do_run_script(AttachContext& ctx, const std::string& script_path) {
    std::ifstream f(script_path);
    if (!f) { std::cerr << "cannot open script: " << script_path << "\n"; return 7; }
    std::stringstream ss; ss << f.rdbuf();
    return do_eval(ctx, ss.str(), 60000);
}

} // anonymous

int wmain(int argc, wchar_t** argv) {
    // Convert wide argv to UTF-8 char*[] for CLI11. Some CLI11 versions reorder
    // the std::vector<std::string> overload, so we use the (argc, argv) form.
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
    attach->add_option("pid", a_pid, "target process ID")->required();
    attach->add_option("--dll", a_dll, "path to payload.dll (default: next to host.exe)");

    auto* run_cmd = app.add_subcommand("run", "attach, execute a JS file, exit");
    uint32_t r_pid = 0;
    std::string r_dll, r_script;
    run_cmd->add_option("pid", r_pid, "target process ID")->required();
    run_cmd->add_option("script", r_script, "path to .js file")->required();
    run_cmd->add_option("--dll", r_dll, "path to payload.dll");

    auto* eval = app.add_subcommand("eval", "attach, evaluate one expression, exit");
    uint32_t e_pid = 0;
    std::string e_dll, e_expr;
    eval->add_option("pid", e_pid, "target process ID")->required();
    eval->add_option("expr", e_expr, "JS expression")->required();
    eval->add_option("--dll", e_dll, "path to payload.dll");

    try { app.parse(argc, argv_utf8.data()); }
    catch (const CLI::ParseError& e) { return app.exit(e); }

    if (*list) return do_list();

    if (*attach) {
        AttachContext ctx{ a_pid, a_dll.empty() ? default_payload_path() : widen(a_dll) };
        return do_attach_repl(ctx);
    }
    if (*run_cmd) {
        AttachContext ctx{ r_pid, r_dll.empty() ? default_payload_path() : widen(r_dll) };
        return do_run_script(ctx, r_script);
    }
    if (*eval) {
        AttachContext ctx{ e_pid, e_dll.empty() ? default_payload_path() : widen(e_dll) };
        return do_eval(ctx, e_expr, 60000);
    }
    return 0;
}
