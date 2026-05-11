#pragma once
#include <wire/wire.h>
#include <functional>
#include <string>

namespace positron::ipc {

using Handler = std::function<void(const positron::wire::Json& cmd)>;
using Greeter = std::function<positron::wire::Json()>;

struct Server {
    static Server& instance();

    // Configure the greeting written to the client immediately after it
    // connects, from the accept thread. Must be set before start().
    void set_greeter(Greeter g);

    void start(uint32_t pid, Handler on_command);
    void push(const positron::wire::Json& msg);
    void stop();
    // Like stop() but does NOT join the accept/writer threads. Safe to call
    // from inside the command handler (which runs on accept_thread itself,
    // so a join would deadlock / be UB). The threads exit naturally as the
    // socket closes and the running flag flips.
    void request_stop();
    bool is_connected() const;
};

} // namespace positron::ipc
