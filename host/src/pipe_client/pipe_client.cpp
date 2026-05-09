#include "pipe_client.h"
#include <Windows.h>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <chrono>

using positron::wire::Json;

namespace positron::pipe {

struct Client::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;
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
            pipe = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
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
        open = true;
        reader = std::thread([this]{ this->run_reader(); });
        return Connected{};
    }

    void run_reader() {
        std::vector<uint8_t> buf;
        std::vector<uint8_t> chunk(8192);
        while (open) {
            DWORD got = 0;
            BOOL ok = ::ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr);
            if (!ok || got == 0) break;
            buf.insert(buf.end(), chunk.data(), chunk.data() + got);
            try {
                while (true) {
                    auto j = wire::frame_try_unpack(buf);
                    if (!j) break;
                    {
                        std::lock_guard lk(mu);
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
        DWORD wrote = 0;
        ::WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
    }

    std::optional<Json> recv(uint32_t timeout_ms) {
        std::unique_lock lk(mu);
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
