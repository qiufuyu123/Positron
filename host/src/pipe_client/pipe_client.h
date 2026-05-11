#pragma once
#include <wire/wire.h>
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <variant>
#include <optional>
#include <cstdint>

namespace positron::pipe {

using EventHandler = std::function<void(const positron::wire::Json& msg)>;

struct ConnectError { std::string message; };
struct Connected { /* opaque */ };
using ConnectResult = std::variant<Connected, ConnectError>;

struct Client {
    Client();
    ~Client();

    ConnectResult connect(uint32_t pid, uint32_t timeout_ms = 5000);
    // Outbound connect to 127.0.0.1:port. v1 uses connect() (host listens,
    // payload connects in). v2 uses connect_to() (target's JS server
    // listens, host connects in).
    ConnectResult connect_to(uint16_t port, uint32_t timeout_ms = 5000);
    void send(const positron::wire::Json&);
    std::optional<positron::wire::Json> recv(uint32_t timeout_ms = 0);
    void close();
    bool is_open() const;

private:
    struct Impl;
    Impl* p;
};

} // namespace positron::pipe
