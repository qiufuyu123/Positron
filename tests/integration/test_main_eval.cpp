#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "fixture_runner.h"
#include "injector/injector.h"
#include "pipe_client/pipe_client.h"
#include <wire/wire.h>
#include <Windows.h>
#include <thread>
#include <chrono>

using namespace positron;

namespace {
struct FixtureGuard {
    test::Fixture fx;
    ~FixtureGuard() { test::kill_fixture(fx); }
};
}

TEST_CASE("attach to main process and eval 1+1 via v8_bridge") {
    // v8_bridge resolves V8 + libuv exports directly from electron.exe so we
    // don't need to inject before the addon is loaded — the bridge works the
    // moment the V8 isolate exists, which is before the JS event loop spins.
    FixtureGuard guard{ test::start_fixture(/*suspended=*/false) };
    auto& fx = guard.fx;
    REQUIRE(fx.main_pid != 0);

    // Give Electron a moment to initialize V8 + uv.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto r = injector::inject({ fx.main_pid, test::abs_payload_dll(), 10000 });
    if (auto* e = std::get_if<injector::Error>(&r)) {
        FAIL("inject failed: ", e->message);
    }

    pipe::Client c;
    auto cr = c.connect(fx.main_pid, 10000);
    if (auto* e = std::get_if<pipe::ConnectError>(&cr)) {
        FAIL("connect failed: ", e->message);
    }

    auto hello = c.recv(10000);
    REQUIRE_MESSAGE(hello, "no HELLO frame received from payload");
    CHECK(hello->value("kind", std::string{}) == "hello");
    MESSAGE("hello: " << hello->dump());

    // The bridge needs the V8 thread to drain its uv_async at least once.
    // Electron's main loop is always running by this point so there's no
    // long delay; 1s is plenty.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    wire::EvalRequest req{1, "1+1", "node", std::nullopt};
    c.send(wire::encode_eval_request(req));

    auto resp = c.recv(60000);
    REQUIRE_MESSAGE(resp, "no eval response received within 60s");
    MESSAGE("response: " << resp->dump());
    auto er = wire::decode_eval_response(*resp);
    REQUIRE(er.ok);
    REQUIRE(er.result);
    CHECK(er.result->type_tag == "number");
    CHECK(er.result->json == "2");

    c.close();
}
