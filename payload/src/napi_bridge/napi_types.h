#pragma once
#include <cstdint>
#include <cstddef>

extern "C" {

struct napi_env__;       using napi_env = napi_env__*;
struct napi_value__;     using napi_value = napi_value__*;
struct napi_handle_scope__; using napi_handle_scope = napi_handle_scope__*;
struct napi_callback_info__; using napi_callback_info = napi_callback_info__*;
struct napi_ref__;       using napi_ref = napi_ref__*;
struct uv_loop_s;        using uv_loop_t = uv_loop_s;

enum napi_status {
    napi_ok = 0,
    napi_invalid_arg,
    napi_object_expected,
    napi_string_expected,
    napi_name_expected,
    napi_function_expected,
    napi_number_expected,
    napi_boolean_expected,
    napi_array_expected,
    napi_generic_failure,
    napi_pending_exception,
    napi_cancelled,
    napi_escape_called_twice,
    napi_handle_scope_mismatch,
    napi_callback_scope_mismatch,
    napi_queue_full,
    napi_closing,
    napi_bigint_expected,
    napi_date_expected,
    napi_arraybuffer_expected,
    napi_detachable_arraybuffer_expected,
    napi_would_deadlock,
    napi_no_external_buffers_allowed,
    napi_cannot_run_js,
};

using napi_register_module_v1_fn = napi_value(__cdecl*)(napi_env, napi_value);

using napi_run_script_fn = napi_status (__cdecl*)(napi_env, napi_value, napi_value*);
using napi_create_string_utf8_fn = napi_status (__cdecl*)(napi_env, const char*, size_t, napi_value*);
using napi_get_and_clear_last_exception_fn = napi_status (__cdecl*)(napi_env, napi_value*);
using napi_open_handle_scope_fn = napi_status (__cdecl*)(napi_env, napi_handle_scope*);
using napi_close_handle_scope_fn = napi_status (__cdecl*)(napi_env, napi_handle_scope);
using napi_get_uv_event_loop_fn = napi_status (__cdecl*)(napi_env, uv_loop_t**);
using napi_get_global_fn = napi_status (__cdecl*)(napi_env, napi_value*);
using napi_get_named_property_fn = napi_status (__cdecl*)(napi_env, napi_value, const char*, napi_value*);
using napi_call_function_fn = napi_status (__cdecl*)(napi_env, napi_value, napi_value, size_t, const napi_value*, napi_value*);
using napi_get_value_string_utf8_fn = napi_status (__cdecl*)(napi_env, napi_value, char*, size_t, size_t*);
using napi_typeof_fn = napi_status (__cdecl*)(napi_env, napi_value, int*);

} // extern "C"
