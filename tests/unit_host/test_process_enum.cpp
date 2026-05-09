#include <doctest/doctest.h>
#include <process_enum/process_enum.h>

using namespace positron::proc;

TEST_CASE("enumerate_electron does not crash and returns sane entries") {
    auto v = enumerate_electron();
    for (auto& e : v) {
        CHECK(e.pid != 0);
        CHECK(e.exe_name.size() > 0);
    }
    CHECK(true); // smoke test: passes whether or not Electron is running
}
