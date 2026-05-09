#pragma once
#include "dispatcher.h"
#include "napi_bridge/napi_bridge.h"
#include <wire/wire.h>
#include <functional>

namespace positron::js {

struct Executor {
    Executor(IDispatcher& d, napi::Bridge& b) : disp(d), b(b) {}

    void eval(const positron::wire::EvalRequest& req,
              std::function<void(positron::wire::EvalResponse)> done);

private:
    IDispatcher& disp;
    napi::Bridge& b;
};

} // namespace positron::js
