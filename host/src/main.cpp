#include "injector/injector.h"
#include "pipe_client/pipe_client.h"
#include <wire/wire.h>
#include <Windows.h>
#include <iostream>
#include <string>
#include <chrono>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { std::wcerr << L"usage: host <pid> <abs-dll-path> [<eval-source>]\n"; return 1; }
    uint32_t pid = std::stoul(argv[1]);
    std::wstring dll = argv[2];
    std::string code = (argc >= 4) ? std::string{} : std::string{};
    if (argc >= 4) {
        // wide -> utf8
        std::wstring w = argv[3];
        int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        code.resize(n - 1);
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, code.data(), n, nullptr, nullptr);
    }

    auto r = positron::injector::inject({pid, dll, 10000});
    if (auto* e = std::get_if<positron::injector::Error>(&r)) {
        std::cerr << "inject FAIL: " << e->message << "\n"; return 2;
    }
    std::cout << "inject OK\n";

    positron::pipe::Client c;
    auto cr = c.connect(pid, 5000);
    if (auto* e = std::get_if<positron::pipe::ConnectError>(&cr)) {
        std::cerr << "connect FAIL: " << e->message << "\n"; return 3;
    }
    auto msg = c.recv(3000);
    if (!msg) { std::cerr << "no HELLO received\n"; return 4; }
    std::cout << "HELLO bytes=" << msg->dump().size() << "\n";

    if (code.empty()) return 0;

    positron::wire::EvalRequest req{1, code, "node", std::nullopt};
    auto t0 = std::chrono::steady_clock::now();
    c.send(positron::wire::encode_eval_request(req));
    std::cout << "sent eval: " << code << "\n";

    auto resp = c.recv(60000);
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (!resp) {
        std::cerr << "no eval response (waited " << ms << "ms)\n";
        return 5;
    }
    std::cout << "eval response after " << ms << "ms: " << resp->dump() << "\n";
    return 0;
}
