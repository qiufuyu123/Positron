#pragma once
#include <wire/wire.h>
#include <functional>
#include <string>

namespace positron::ipc {

using Handler = std::function<void(const positron::wire::Json& cmd)>;

struct Server {
    static Server& instance();

    void start(uint32_t pid, Handler on_command);
    void push(const positron::wire::Json& msg);
    void stop();
    bool is_connected() const;
};

} // namespace positron::ipc
