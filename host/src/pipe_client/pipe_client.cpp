#include "pipe_client.h"
// WinSock2 must come before Windows.h (or use WIN32_LEAN_AND_MEAN).
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <chrono>
#include <atomic>

#pragma comment(lib, "Ws2_32.lib")

using positron::wire::Json;

// Transport: TCP socket on 127.0.0.1. Chosen over named pipes because the
// payload running inside a Chromium renderer sandbox cannot create named
// pipes (CreateNamedPipeW is blocked by sandbox lockdown) but CAN open a
// TCP socket and connect() to loopback.
//
// Roles inverted from naming:
//   * HOST is the SERVER (this file). Listens, accepts the payload's
//     incoming connection.
//   * PAYLOAD is the CLIENT — connects out to localhost:<port>.
//
// Port is deterministic from the target PID so the payload (which runs the
// same calculation against GetCurrentProcessId()) can find us without any
// out-of-band parameter passing.

namespace positron::pipe {

static uint16_t port_for_pid(uint32_t pid) {
    return static_cast<uint16_t>(30000u + (pid % 30000u));
}

struct Wsa {
    Wsa()  { WSADATA d; ::WSAStartup(MAKEWORD(2, 2), &d); }
    ~Wsa() { ::WSACleanup(); }
};
static Wsa g_wsa;

struct Client::Impl {
    SOCKET listener = INVALID_SOCKET;
    SOCKET conn     = INVALID_SOCKET;
    std::mutex write_mu;
    std::thread reader;
    std::mutex mu;
    std::condition_variable cv;
    std::queue<Json> inbox;
    std::atomic<bool> open{false};

    ~Impl() { close(); }

    ConnectResult connect_to(uint16_t port, uint32_t timeout_ms) {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return ConnectError{"socket() failed: " + std::to_string(::WSAGetLastError())};

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        u_long nb = 1;
        ::ioctlsocket(s, FIONBIO, &nb);
        int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc != 0) {
            int err = ::WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                ::closesocket(s);
                return ConnectError{"connect(127.0.0.1:" + std::to_string(port) +
                                    ") failed: " + std::to_string(err)};
            }
            fd_set ws; FD_ZERO(&ws); FD_SET(s, &ws);
            timeval tv{};
            tv.tv_sec  = static_cast<long>(timeout_ms / 1000);
            tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
            int sel = ::select(0, nullptr, &ws, nullptr, &tv);
            if (sel <= 0) {
                ::closesocket(s);
                return ConnectError{"connect timeout to 127.0.0.1:" + std::to_string(port)};
            }
            int so_err = 0; int so_len = sizeof(so_err);
            ::getsockopt(s, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&so_err), &so_len);
            if (so_err != 0) {
                ::closesocket(s);
                return ConnectError{"connect failed: " + std::to_string(so_err)};
            }
        }
        nb = 0;
        ::ioctlsocket(s, FIONBIO, &nb);

        BOOL nodelay = TRUE;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        conn = s;
        open = true;
        reader = std::thread([this]{ this->run_reader(); });
        return Connected{};
    }

    ConnectResult connect(uint32_t pid, uint32_t timeout_ms) {
        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
            return ConnectError{"socket() failed: " + std::to_string(::WSAGetLastError())};

        BOOL reuse = TRUE;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port_for_pid(pid));

        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            int err = ::WSAGetLastError();
            ::closesocket(listener); listener = INVALID_SOCKET;
            return ConnectError{"bind(127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) +
                                ") failed: " + std::to_string(err)};
        }

        if (::listen(listener, 1) == SOCKET_ERROR) {
            int err = ::WSAGetLastError();
            ::closesocket(listener); listener = INVALID_SOCKET;
            return ConnectError{"listen() failed: " + std::to_string(err)};
        }

        // accept() with timeout via select.
        fd_set rs; FD_ZERO(&rs); FD_SET(listener, &rs);
        timeval tv{};
        tv.tv_sec  = static_cast<long>(timeout_ms / 1000);
        tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
        int sel = ::select(0, &rs, nullptr, nullptr, &tv);
        if (sel <= 0) {
            ::closesocket(listener); listener = INVALID_SOCKET;
            return ConnectError{"timeout waiting for payload to connect on port " +
                                std::to_string(port_for_pid(pid))};
        }

        sockaddr_in peer{};
        int peer_len = sizeof(peer);
        conn = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        ::closesocket(listener); listener = INVALID_SOCKET;
        if (conn == INVALID_SOCKET) {
            return ConnectError{"accept() failed: " + std::to_string(::WSAGetLastError())};
        }

        BOOL nodelay = TRUE;
        ::setsockopt(conn, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        open = true;
        reader = std::thread([this]{ this->run_reader(); });
        return Connected{};
    }

    void run_reader() {
        std::vector<uint8_t> buf;
        std::vector<uint8_t> chunk(8192);
        while (open) {
            int got = ::recv(conn, reinterpret_cast<char*>(chunk.data()),
                             static_cast<int>(chunk.size()), 0);
            if (got <= 0) break;
            buf.insert(buf.end(), chunk.data(), chunk.data() + got);
            try {
                while (true) {
                    auto j = wire::frame_try_unpack(buf);
                    if (!j) break;
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        inbox.push(*j);
                    }
                    cv.notify_all();
                }
            } catch (...) { break; }
        }
        open = false;
        cv.notify_all();
    }

    void send(const Json& j) {
        if (!open) return;
        auto bytes = wire::frame_pack(j);
        std::lock_guard<std::mutex> lk(write_mu);
        const char* p = reinterpret_cast<const char*>(bytes.data());
        int remain = static_cast<int>(bytes.size());
        while (remain > 0) {
            int n = ::send(conn, p, remain, 0);
            if (n <= 0) break;
            p += n; remain -= n;
        }
    }

    std::optional<Json> recv(uint32_t timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        if (timeout_ms == 0) {
            // Non-blocking peek: return immediately if nothing queued.
            if (inbox.empty()) return std::nullopt;
        } else {
            cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                [this]{ return !inbox.empty() || !open; });
        }
        if (inbox.empty()) return std::nullopt;
        Json j = std::move(inbox.front()); inbox.pop();
        return j;
    }

    void close() {
        bool was = open.exchange(false);
        if (conn != INVALID_SOCKET) {
            ::shutdown(conn, SD_BOTH);
            ::closesocket(conn);
            conn = INVALID_SOCKET;
        }
        if (listener != INVALID_SOCKET) {
            ::closesocket(listener);
            listener = INVALID_SOCKET;
        }
        if (reader.joinable()) reader.join();
        cv.notify_all();
        (void)was;
    }
};

Client::Client() : p(new Impl) {}
Client::~Client() { delete p; }
ConnectResult Client::connect(uint32_t pid, uint32_t timeout_ms) { return p->connect(pid, timeout_ms); }
ConnectResult Client::connect_to(uint16_t port, uint32_t timeout_ms) { return p->connect_to(port, timeout_ms); }
void Client::send(const Json& j) { p->send(j); }
std::optional<Json> Client::recv(uint32_t timeout_ms) { return p->recv(timeout_ms); }
void Client::close() { p->close(); }
bool Client::is_open() const { return p->open.load(); }

} // namespace positron::pipe
