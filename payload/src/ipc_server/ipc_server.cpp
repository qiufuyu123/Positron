#include "pch.h"
#include "ipc_server.h"
#include "logging/log.h"
#include <Windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

using positron::wire::Json;

namespace positron::ipc {

struct Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::thread accept_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    Handler handler;
    Greeter greeter;
    std::mutex write_mu;

    std::wstring pipe_name(uint32_t pid) {
        return L"\\\\.\\pipe\\positron-" + std::to_wstring(pid);
    }

    void start(uint32_t pid, Handler h) {
        handler = std::move(h);
        running = true;
        accept_thread = std::thread([this, pid]{ this->run(pid); });
    }

    void run(uint32_t pid) {
        auto name = pipe_name(pid);
        pipe = ::CreateNamedPipeW(name.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 64*1024, 64*1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            log::error("CreateNamedPipeW failed: " + std::to_string(::GetLastError()));
            return;
        }
        log::info("pipe listening: \\\\.\\pipe\\positron-" + std::to_string(pid));

        BOOL ok = ::ConnectNamedPipe(pipe, nullptr) ? TRUE : (::GetLastError() == ERROR_PIPE_CONNECTED);
        if (!ok) { log::error("ConnectNamedPipe failed"); return; }
        connected = true;
        log::info("pipe client connected");

        // Send the connection greeting (e.g. HELLO) from the same thread that
        // owns the pipe handle. WriteFile-from-another-thread on a synchronous
        // pipe handle does not interact correctly with a concurrent ReadFile
        // on the same thread that called ConnectNamedPipe, so we serialise the
        // greeting here before entering the read loop.
        if (greeter) {
            try {
                auto greeting = greeter();
                auto bytes = wire::frame_pack(greeting);
                std::lock_guard lk(write_mu);
                DWORD wrote = 0;
                BOOL gok = ::WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
                if (!gok) {
                    log::error("greeter WriteFile failed: " + std::to_string(::GetLastError()));
                } else {
                    log::info("greeter sent " + std::to_string(wrote) + " bytes");
                }
            } catch (const std::exception& e) {
                log::error(std::string("greeter threw: ") + e.what());
            }
        }

        std::vector<uint8_t> buf;
        buf.reserve(64*1024);
        std::vector<uint8_t> chunk(8192);

        while (running) {
            DWORD got = 0;
            if (!::ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) || got == 0) {
                log::info("pipe disconnected");
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
        ::CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }

    void push(const Json& j) {
        if (!connected) return;
        auto bytes = wire::frame_pack(j);
        std::lock_guard lk(write_mu);
        DWORD wrote = 0;
        if (!::WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr)) {
            log::error("push WriteFile failed: " + std::to_string(::GetLastError()));
        }
    }

    void stop() {
        running = false;
        if (pipe != INVALID_HANDLE_VALUE) ::DisconnectNamedPipe(pipe);
        if (accept_thread.joinable()) accept_thread.join();
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
