#include "pch.h"
#include "ipc_server.h"
#include "logging/log.h"
#include <Windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>

using positron::wire::Json;

namespace positron::ipc {

// FILE_FLAG_OVERLAPPED on the pipe so reader thread's ReadFile and writer
// thread's WriteFile do not serialise on the kernel's per-handle lock.

struct Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE read_evt = nullptr;
    HANDLE write_evt = nullptr;
    HANDLE connect_evt = nullptr;
    std::thread accept_thread;
    std::thread writer_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    Handler handler;
    Greeter greeter;

    std::mutex outbox_mu;
    std::condition_variable outbox_cv;
    std::queue<std::vector<uint8_t>> outbox;

    std::wstring pipe_name(uint32_t pid) {
        return L"\\\\.\\pipe\\positron-" + std::to_wstring(pid);
    }

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
            if (!connected || pipe == INVALID_HANDLE_VALUE) continue;
            log::info("writer: WriteFile " + std::to_string(bytes.size()) + " bytes");
            OVERLAPPED ov{};
            ov.hEvent = write_evt;
            ::ResetEvent(write_evt);
            DWORD wrote = 0;
            BOOL ok = ::WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, &ov);
            if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
                if (::GetOverlappedResult(pipe, &ov, &wrote, TRUE)) ok = TRUE;
            }
            if (!ok) {
                log::error("push WriteFile failed: " + std::to_string(::GetLastError()));
            } else {
                log::info("writer: WriteFile ok wrote=" + std::to_string(wrote));
            }
        }
    }

    void run(uint32_t pid) {
        auto name = pipe_name(pid);
        pipe = ::CreateNamedPipeW(name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 1024 * 1024, 1024 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            log::error("CreateNamedPipeW failed: " + std::to_string(::GetLastError()));
            return;
        }
        read_evt    = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        write_evt   = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        connect_evt = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        log::info("pipe listening: \\\\.\\pipe\\positron-" + std::to_string(pid));

        // Async ConnectNamedPipe.
        OVERLAPPED ov_connect{};
        ov_connect.hEvent = connect_evt;
        BOOL connected_now = ::ConnectNamedPipe(pipe, &ov_connect);
        if (!connected_now) {
            DWORD err = ::GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected_now = TRUE;
            } else if (err == ERROR_IO_PENDING) {
                DWORD got = 0;
                if (::GetOverlappedResult(pipe, &ov_connect, &got, TRUE)) connected_now = TRUE;
            } else {
                log::error("ConnectNamedPipe failed: " + std::to_string(err));
                return;
            }
        }
        if (!connected_now) { log::error("ConnectNamedPipe gave up"); return; }
        connected = true;
        log::info("pipe client connected");

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
            OVERLAPPED ov{};
            ov.hEvent = read_evt;
            ::ResetEvent(read_evt);
            DWORD got = 0;
            BOOL rok = ::ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &got, &ov);
            if (!rok && ::GetLastError() == ERROR_IO_PENDING) {
                if (!::GetOverlappedResult(pipe, &ov, &got, TRUE)) {
                    log::info("pipe disconnected (overlapped read failed)");
                    break;
                }
                rok = TRUE;
            }
            if (!rok || got == 0) {
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
        if (pipe != INVALID_HANDLE_VALUE) {
            ::CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
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
        if (pipe != INVALID_HANDLE_VALUE) ::DisconnectNamedPipe(pipe);
        if (accept_thread.joinable()) accept_thread.join();
        if (writer_thread.joinable()) writer_thread.join();
        if (read_evt)    { ::CloseHandle(read_evt);    read_evt = nullptr; }
        if (write_evt)   { ::CloseHandle(write_evt);   write_evt = nullptr; }
        if (connect_evt) { ::CloseHandle(connect_evt); connect_evt = nullptr; }
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
