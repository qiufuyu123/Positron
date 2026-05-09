#pragma once
#include "napi_bridge/napi_types.h"
#include <functional>

namespace positron::js {

struct IDispatcher {
    virtual ~IDispatcher() = default;
    virtual void run_on_v8_thread(std::function<void(napi_env)> fn) = 0;
    virtual napi_env env_unsafe() = 0;
};

struct InlineDispatcher : IDispatcher {
    napi_env env;
    explicit InlineDispatcher(napi_env e) : env(e) {}
    void run_on_v8_thread(std::function<void(napi_env)> fn) override { fn(env); }
    napi_env env_unsafe() override { return env; }
};

} // namespace positron::js
