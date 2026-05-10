#include <doctest/doctest.h>
#include <hook_engine/ring_buffer.h>
#include <cstring>
#include <thread>
#include <atomic>

using positron::hook::RingBuffer;

TEST_CASE("ring buffer basic push/pop") {
    RingBuffer<256> rb;
    const char msg[] = "hello";
    REQUIRE(rb.push(msg, sizeof(msg)));
    char buf[16] = {};
    auto n = rb.pop(buf, sizeof(buf));
    CHECK(n == sizeof(msg));
    CHECK(std::strcmp(buf, msg) == 0);
}

TEST_CASE("ring buffer rejects when full") {
    RingBuffer<32> rb;
    char big[20] = {};
    REQUIRE(rb.push(big, sizeof(big)));      // 4 + 20 = 24 bytes used
    CHECK_FALSE(rb.push(big, sizeof(big)));  // would need another 24, only 8 free
}

TEST_CASE("ring buffer wraps around") {
    RingBuffer<32> rb;
    char data[10];
    for (int i = 0; i < 10; ++i) data[i] = static_cast<char>(i);

    // Push and immediately pop several times — wraps around the 32-byte buffer.
    for (int i = 0; i < 20; ++i) {
        REQUIRE(rb.push(data, sizeof(data)));
        char out[10] = {};
        auto n = rb.pop(out, sizeof(out));
        CHECK(n == sizeof(data));
        CHECK(std::memcmp(out, data, sizeof(data)) == 0);
    }
}

TEST_CASE("ring buffer SPSC concurrency") {
    RingBuffer<4096> rb;
    constexpr int N = 10000;
    std::atomic<int> received{0};

    std::thread producer([&]{
        for (int i = 0; i < N; ++i) {
            while (!rb.push(&i, sizeof(i))) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]{
        int expected = 0;
        while (expected < N) {
            int v = -1;
            auto n = rb.pop(&v, sizeof(v));
            if (n == 0) { std::this_thread::yield(); continue; }
            CHECK(n == sizeof(int));
            CHECK(v == expected);
            ++expected;
            ++received;
        }
    });

    producer.join();
    consumer.join();
    CHECK(received.load() == N);
}
