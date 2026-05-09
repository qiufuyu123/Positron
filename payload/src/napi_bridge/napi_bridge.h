#pragma once
#include "napi_types.h"
#include "pe_resolver/pe_resolver.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <vector>
#include <optional>

namespace positron::napi {

struct Bridge {
    static Bridge& instance();

    bool initialize();
    bool has_env() const;
    void run_on_v8_thread(std::function<void(napi_env)> fn);
    napi_env env_unsafe() const;

    napi_run_script_fn run_script = nullptr;
    napi_create_string_utf8_fn create_string_utf8 = nullptr;
    napi_get_and_clear_last_exception_fn get_and_clear_last_exception = nullptr;
    napi_open_handle_scope_fn open_handle_scope = nullptr;
    napi_close_handle_scope_fn close_handle_scope = nullptr;
    napi_get_uv_event_loop_fn get_uv_event_loop = nullptr;
    napi_get_global_fn get_global = nullptr;
    napi_get_named_property_fn get_named_property = nullptr;
    napi_call_function_fn call_function = nullptr;
    napi_get_value_string_utf8_fn get_value_string_utf8 = nullptr;
    napi_typeof_fn typeof_ = nullptr;

    std::vector<std::string> present_symbols;

    pe::ExportTable cached_table;

    bool is_renderer() const;
    std::optional<uintptr_t> symbol_addr(const std::string& name) const;
    void stop_kicker();
};

} // namespace positron::napi
