#include "injector/injector.h"
#include "pipe_client/pipe_client.h"
#include <wire/wire.h>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::wcerr << L"usage: host <pid> <abs-dll-path>\n"; return 1; }
    uint32_t pid = std::stoul(argv[1]);
    std::wstring dll = argv[2];

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
    std::cout << "HELLO: " << msg->dump() << "\n";
    return 0;
}
