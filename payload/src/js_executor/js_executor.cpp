#include "pch.h"
#include "js_executor.h"
#include "logging/log.h"
#include <vector>
#include <string>

using namespace positron::wire;

namespace positron::js {

static std::string read_string_value(positron::napi::Bridge& b, napi_env env, napi_value v) {
    size_t len = 0;
    if (b.get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) return {};
    std::string s(len, '\0');
    size_t copied = 0;
    if (b.get_value_string_utf8(env, v, s.data(), len + 1, &copied) != napi_ok) return {};
    s.resize(copied);
    return s;
}

static std::string json_stringify(positron::napi::Bridge& b, napi_env env, napi_value value) {
    napi_value global = nullptr;
    if (b.get_global(env, &global) != napi_ok) return "null";
    napi_value json_obj = nullptr;
    if (b.get_named_property(env, global, "JSON", &json_obj) != napi_ok) return "null";
    napi_value stringify = nullptr;
    if (b.get_named_property(env, json_obj, "stringify", &stringify) != napi_ok) return "null";
    napi_value args[1] = { value };
    napi_value result = nullptr;
    if (b.call_function(env, json_obj, stringify, 1, args, &result) != napi_ok) return "null";
    return read_string_value(b, env, result);
}

static const char* type_tag_of(int t) {
    switch (t) {
        case 0: return "undefined";
        case 1: return "null";
        case 2: return "boolean";
        case 3: return "number";
        case 4: return "string";
        case 5: return "symbol";
        case 6: return "object";
        case 7: return "function";
        case 8: return "external";
        case 9: return "bigint";
        default: return "unknown";
    }
}

void Executor::eval(const EvalRequest& req,
                    std::function<void(EvalResponse)> done) {
    auto& bridge = b;
    disp.run_on_v8_thread([req, &bridge, done = std::move(done)](napi_env env) mutable {
        EvalResponse resp;
        resp.id = req.id;
        if (!env || !bridge.run_script || !bridge.create_string_utf8) {
            resp.ok = false;
            resp.error = EvalError{"napi_env not captured", ""};
            done(std::move(resp));
            return;
        }
        napi_handle_scope scope = nullptr;
        bridge.open_handle_scope(env, &scope);

        napi_value src = nullptr;
        if (bridge.create_string_utf8(env, req.code.c_str(), req.code.size(), &src) != napi_ok) {
            resp.ok = false;
            resp.error = EvalError{"create_string_utf8 failed", ""};
            bridge.close_handle_scope(env, scope);
            done(std::move(resp)); return;
        }
        napi_value out = nullptr;
        auto rc = bridge.run_script(env, src, &out);
        if (rc != napi_ok) {
            napi_value exc = nullptr;
            bridge.get_and_clear_last_exception(env, &exc);
            EvalError err;
            if (exc) {
                napi_value msg_v = nullptr, stk_v = nullptr;
                bridge.get_named_property(env, exc, "message", &msg_v);
                bridge.get_named_property(env, exc, "stack", &stk_v);
                if (msg_v) err.message = read_string_value(bridge, env, msg_v);
                if (stk_v) err.stack = read_string_value(bridge, env, stk_v);
            } else {
                err.message = "napi_run_script returned status " + std::to_string(rc);
            }
            resp.ok = false; resp.error = std::move(err);
            bridge.close_handle_scope(env, scope);
            done(std::move(resp)); return;
        }
        int t = 0;
        bridge.typeof_(env, out, &t);
        EvalResult r;
        r.type_tag = type_tag_of(t);
        r.json = json_stringify(bridge, env, out);
        resp.ok = true; resp.result = std::move(r);
        bridge.close_handle_scope(env, scope);
        done(std::move(resp));
    });
}

} // namespace positron::js
