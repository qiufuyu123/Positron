// Minimal positron SDK consumer.
//   eval_example list                    — list candidate Electron processes
//   eval_example eval <pid> "<expr>"     — attach, eval, print, detach
//   eval_example renderer <pid> "<expr>" — same but hop into renderer's main world
//
// Build: link positron_sdk.lib + BlackBone.lib + dbghelp.lib + Ws2_32.lib.

#include <positron/sdk.h>

#include <Windows.h>
#include <iostream>
#include <string>

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static int do_list() {
    auto procs = positron::sdk::list_processes();
    if (procs.empty()) { std::cerr << "no electron processes found\n"; return 0; }
    std::wcout << L"PID\tTYPE\tEXE\n";
    for (auto& p : procs) {
        std::wcout << p.pid << L"\t"
                   << positron::sdk::type_name(p.type) << L"\t"
                   << p.exe_name << L"\n";
    }
    return 0;
}

static int do_eval(uint32_t pid, const std::string& expr, bool renderer) {
    positron::sdk::Session s;
    if (auto err = s.attach(pid)) {
        std::cerr << "attach failed: " << err->message << "\n";
        return 2;
    }

    positron::sdk::EvalOptions opts;
    if (renderer) opts.world = positron::sdk::World::Renderer;

    auto r = s.eval(expr, opts);
    if (r.ok) {
        std::cout << r.json_value << "\n";
        s.detach();
        return 0;
    }
    std::cerr << "error: " << r.error_message << "\n";
    if (!r.error_stack.empty()) std::cerr << r.error_stack << "\n";
    s.detach();
    return 3;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage:\n"
                  << "  " << argv[0] << " list\n"
                  << "  " << argv[0] << " eval     <pid> \"<expr>\"\n"
                  << "  " << argv[0] << " renderer <pid> \"<expr>\"\n";
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "list") return do_list();
    if ((cmd == "eval" || cmd == "renderer") && argc >= 4) {
        uint32_t pid = std::stoul(argv[2]);
        return do_eval(pid, argv[3], cmd == "renderer");
    }
    std::cerr << "bad arguments\n";
    return 1;
}
