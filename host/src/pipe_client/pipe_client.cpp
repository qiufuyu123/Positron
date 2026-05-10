#include "pipe_client.h"
#include <Windows.h>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <chrono>
#include <atomic>

using positron::wire::Json;

namespace positron::pipe {

// We use FILE_FLAG_OVERLAPPED on the pipe handle so that the reader thread's
// ReadFile and the sender's WriteFile do not serialise on the per-handle
// kernel lock. Without OVERLAPPED, a long-lived blocking ReadFile would
// hold the handle and block any concurrent WriteFile from another thread,
// producing a hard deadlock with the server-side payload.

struct Client::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE read_evt = nullptr;
    HANDLE write_evt = nullptr;
    std::mutex write_mu;
    std::thread reader;
    std::mutex mu;
    std::condition_variable cv;
    std::queue<Json> inbox;
    std::atomic<bool> open{false};

    ~Impl() { close(); }

    ConnectResult connect(uint32_t pid, uint32_t timeout_ms) {
        std::wstring name = L"\\\\.\\pipe\\positron-" + std::to_wstring(pid);
        DWORD start = ::GetTickCount();
        while (true) {
            pipe = ::CreateFileW(name.c_str(),
                                 GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            DWORD err = ::GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                ::WaitNamedPipeW(name.c_str(), 200);
            } else if (err == ERROR_FILE_NOT_FOUND) {
                ::Sleep(100);
            } else {
                return ConnectError{"CreateFileW failed: " + std::to_string(err)};
            }
            if (::GetTickCount() - start > timeout_ms) {
                return ConnectError{"timeout connecting to pipe"};
            }
        }
        DWORD mode = PIPE_READMODE_BYTE;
        ::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
        read_evt  = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        write_evt = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        open = true;
        reader = std::thread([this]{ this->run_reader(); });
        return Connected{};
    }

    void run_reader() {
        std::vector<uint8_t> buf;
        std::vector<uint8_t> chunk(8192);
        while (open) {
            OVERLAPPED ov{};
            ov.hEvent = read_evt;
            ::ResetEvent(read_evt);
            DWORD got = 0;
            BOOL ok = ::ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &got, &ov);
            if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
                if (!::GetOverlappedResult(pipe, &ov, &got, TRUE)) break;
                ok = TRUE;
            }
            if (!ok || got == 0) break;
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
        OVERLAPPED ov{};
        ov.hEvent = write_evt;
        ::ResetEvent(write_evt);
        DWORD wrote = 0;
        BOOL ok = ::WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, &ov);
        if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
            ::GetOverlappedResult(pipe, &ov, &wrote, TRUE);
        }
    }

    std::optional<Json> recv(uint32_t timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        if (timeout_ms == 0) {
            cv.wait(lk, [this]{ return !inbox.empty() || !open; });
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
        if (pipe != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(pipe, nullptr);
            ::CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (reader.joinable()) reader.join();
        if (read_evt)  { ::CloseHandle(read_evt);  read_evt  = nullptr; }
        if (write_evt) { ::CloseHandle(write_evt); write_evt = nullptr; }
        cv.notify_all();
        (void)was;
    }
};

Client::Client() : p(new Impl) {}
Client::~Client() { delete p; }
ConnectResult Client::connect(uint32_t pid, uint32_t timeout_ms) { return p->connect(pid, timeout_ms); }
void Client::send(const Json& j) { p->send(j); }
std::optional<Json> Client::recv(uint32_t timeout_ms) { return p->recv(timeout_ms); }
void Client::close() { p->close(); }
bool Client::is_open() const { return p->open.load(); }

} // namespace positron::pipe
