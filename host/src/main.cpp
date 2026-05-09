#include "injector/injector.h"
#include <Windows.h>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::wcerr << L"usage: host <pid> <abs-dll-path>\n"; return 1; }
    positron::injector::Options o;
    o.pid = std::stoul(argv[1]);
    o.dll_absolute_path = argv[2];
    auto r = positron::injector::inject(o);
    if (auto* e = std::get_if<positron::injector::Error>(&r)) {
        std::cerr << "FAIL: " << e->message << "\n"; return 2;
    }
    std::cout << "OK: module base 0x" << std::hex << std::get<positron::injector::Success>(r).module_base << "\n";
    return 0;
}
