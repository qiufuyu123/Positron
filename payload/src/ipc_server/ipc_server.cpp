#include "pch.h"
#include "ipc_server.h"
#include "logging/log.h"
// WinSock2 must come before Windows.h.
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <chrono>

#pragma comment(lib, "Ws2_32.lib")

using positron::wire::Json;

// Transport: TCP socket to 127.0.0.1:<port>. Replaces named pipes because the
// Chromium renderer sandbox blocks pipe creation; loopback connect() is still
// allowed.
//
// Roles inverted from the class name: this "Server" actually CONNECTS as a
// client to a host-side listener. The naming is kept to avoid churning every
// caller — see comments at the bottom for the public contract.

namespace positron::ipc {

static uint16_t port_for_pid(uint32_t pid) {
    return static_cast<uint16_t>(30000u + (pid % 30000u));
}

struct Wsa {
    Wsa()  { WSADATA d; ::WSAStartup(MAKEWORD(2, 2), &d); }
    ~Wsa() { ::WSACleanup(); }
};
static Wsa g_wsa;

struct Impl {
    SOCKET conn = INVALID_SOCKET;
    std::thread accept_thread;   // historical name; actually the connect+read thread
    std::thread writer_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    Handler handler;
    Greeter greeter;

    std::mutex outbox_mu;
    std::condition_variable outbox_cv;
    std::queue<std::vector<uint8_t>> outbox;

    void start(uint32_t pid, Handler h) {
        handler = std::move(h);
        running = true;
        accept_thread = std::thread([this, pid]{ this->run(pid); });
        writer_thread = std::thread([this]{ this->writer_loop(); });
    }

    void writer_loop() {
        while (running) {
            std::vector<uint8_t> bytes;
            {
                std::unique_lock<std::mutex> lk(outbox_mu);
                outbox_cv.wait(lk, [this]{ return !outbox.empty() || !running; });
                if (!running && outbox.empty()) return;
                bytes = std::move(outbox.front());
                outbox.pop();
            }
            if (!connected || conn == INVALID_SOCKET) continue;
            const char* p = reinterpret_cast<const char*>(bytes.data());
            int remain = static_cast<int>(bytes.size());
            while (remain > 0) {
                int n = ::send(conn, p, remain, 0);
                if (n <= 0) {
                    log::error("send failed: " + std::to_string(::WSAGetLastError()));
                    break;
                }
                p += n; remain -= n;
            }
        }
    }

    void run(uint32_t pid) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port_for_pid(pid));

        int attempts = 0;
        constexpr int max_attempts = 50;
        while (running && attempts < max_attempts) {
            ++attempts;
            conn = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (conn == INVALID_SOCKET) {
                log::error("socket() failed: " + std::to_string(::WSAGetLastError()));
                return;
            }
            if (::connect(conn, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                break;
            }
            int err = ::WSAGetLastError();
            ::closesocket(conn);
            conn = INVALID_SOCKET;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (attempts == 1 || attempts == max_attempts) {
                log::warn("connect attempt " + std::to_string(attempts) +
                          " to 127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) +
                          " err=" + std::to_string(err));
            }
        }
        if (conn == INVALID_SOCKET) {
            log::error("could not connect to host within timeout");
            return;
        }

        BOOL nodelay = TRUE;
        ::setsockopt(conn, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        connected = true;
        log::info("connected to host on port " + std::to_string(ntohs(addr.sin_port)));

        if (greeter) {
            try {
                auto greeting = greeter();
                push(greeting);
                log::info("greeter queued");
            } catch (const std::exception& e) {
                log::error(std::string("greeter threw: ") + e.what());
            }
        }

        std::vector<uint8_t> buf;
        buf.reserve(64 * 1024);
        std::vector<uint8_t> chunk(8192);

        while (running) {
            int got = ::recv(conn, reinterpret_cast<char*>(chunk.data()),
                             static_cast<int>(chunk.size()), 0);
            if (got <= 0) {
                log::info("socket disconnected");
                break;
            }
            buf.insert(buf.end(), chunk.data(), chunk.data() + got);
            try {
                while (true) {
                    auto j = wire::frame_try_unpack(buf);
                    if (!j) break;
                    if (handler) handler(*j);
                }
            } catch (const std::exception& e) {
                log::error(std::string("frame parse error: ") + e.what());
                break;
            }
        }
        connected = false;
        if (conn != INVALID_SOCKET) {
            ::closesocket(conn);
            conn = INVALID_SOCKET;
        }
    }

    void push(const Json& j) {
        if (!connected) return;
        auto bytes = wire::frame_pack(j);
        {
            std::lock_guard<std::mutex> lk(outbox_mu);
            outbox.push(std::move(bytes));
        }
        outbox_cv.notify_one();
    }

    void stop() {
        running = false;
        outbox_cv.notify_all();
        if (conn != INVALID_SOCKET) ::shutdown(conn, SD_BOTH);
        if (accept_thread.joinable()) accept_thread.join();
        if (writer_thread.joinable()) writer_thread.join();
    }
};

Server& Server::instance() {
    static Server s;
    return s;
}

static Impl g_impl;

void Server::set_greeter(Greeter g) { g_impl.greeter = std::move(g); }
void Server::start(uint32_t pid, Handler h) { g_impl.start(pid, std::move(h)); }
void Server::push(const Json& j) { g_impl.push(j); }
void Server::stop() { g_impl.stop(); }
bool Server::is_connected() const { return g_impl.connected.load(); }

} // namespace positron::ipc
