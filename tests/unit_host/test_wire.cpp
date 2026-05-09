#include <doctest/doctest.h>
#include <wire/wire.h>

using namespace positron::wire;

TEST_CASE("frame round-trip") {
    Json j{{"hello","world"},{"n",42}};
    auto bytes = frame_pack(j);
    REQUIRE(bytes.size() >= 4);

    std::vector<uint8_t> buf(bytes.begin(), bytes.end());
    auto out = frame_try_unpack(buf);
    REQUIRE(out.has_value());
    CHECK(out->at("hello") == "world");
    CHECK(out->at("n") == 42);
    CHECK(buf.empty());
}

TEST_CASE("frame partial unpack returns nullopt") {
    Json j{{"a",1}};
    auto bytes = frame_pack(j);
    std::vector<uint8_t> buf(bytes.begin(), bytes.begin() + 4);
    auto out = frame_try_unpack(buf);
    CHECK_FALSE(out.has_value());
    CHECK(buf.size() == 4);
}

TEST_CASE("hello round-trip") {
    Hello h{"30.0.0", "main", {"napi_throw","napi_run_script"}, 1234};
    auto j = encode_hello(h);
    auto h2 = decode_hello(j);
    CHECK(h2.electron_version == "30.0.0");
    CHECK(h2.target_type == "main");
    CHECK(h2.napi_symbols_present.size() == 2);
    CHECK(h2.pid == 1234);
}

TEST_CASE("eval request/response round-trip") {
    EvalRequest req{42, "1+1", "main", std::nullopt};
    auto j = encode_eval_request(req);
    auto r = decode_eval_request(j);
    CHECK(r.id == 42);
    CHECK(r.code == "1+1");

    EvalResponse resp{42, true, EvalResult{"number","2"}, std::nullopt};
    auto rj = encode_eval_response(resp);
    auto rr = decode_eval_response(rj);
    CHECK(rr.ok);
    REQUIRE(rr.result);
    CHECK(rr.result->type_tag == "number");
    CHECK(rr.result->json == "2");
}
