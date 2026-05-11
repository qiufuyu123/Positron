#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "fixture_runner.h"
#include <positron/sdk.h>
#include <Windows.h>
#include <chrono>
#include <thread>

namespace {
struct FixtureGuard {
    positron::test::Fixture fx;
    ~FixtureGuard() { positron::test::kill_fixture(fx); }
};
}

TEST_CASE("attach to main process and eval 1+1 via SDK") {
    FixtureGuard guard{ positron::test::start_fixture(/*suspended=*/false) };
    auto& fx = guard.fx;
    REQUIRE(fx.main_pid != 0);

    // Give Electron a moment to initialise V8 + uv.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    positron::sdk::Session s;
    // Pin to Native transport — v2 has its own coverage; this test guards
    // the v1 path that still backs `host.exe attach --native`.
    auto err = s.attach(fx.main_pid, positron::test::abs_payload_dll(),
                        positron::sdk::Transport::Native);
    if (err) FAIL("attach failed: ", err->message);

    // First eval may need a brief settle while the bridge arms uv_async on V8.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto r = s.eval("1+1");
    REQUIRE(r.ok);
    CHECK(r.type_tag == "number");
    CHECK(r.json_value == "2");

    s.detach();
}
