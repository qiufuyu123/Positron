#pragma once
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace positron::wire {

using Json = nlohmann::json;

constexpr uint32_t kMaxFrameBytes = 16 * 1024 * 1024;

enum class CmdKind {
    Hello, Eval, HookInstall, HookUninstall, Detach, WorldList,
};

enum class EventKind {
    Ready, HookHit, HookScriptError, Log,
};

struct Hello {
    std::string electron_version;
    std::string target_type;
    std::vector<std::string> napi_symbols_present;
    uint32_t pid;
};

struct EvalRequest {
    uint64_t id;
    std::string code;
    std::string world;
    std::optional<int32_t> world_index;
};

struct EvalResult {
    std::string type_tag;
    std::string json;
};

struct EvalError {
    std::string message;
    std::string stack;
};

struct EvalResponse {
    uint64_t id;
    bool ok;
    std::optional<EvalResult> result;
    std::optional<EvalError> error;
};

struct HookInstallRequest {
    uint64_t id;
    std::string symbol;
    std::string script;
};

struct HookHitEvent {
    uint64_t hook_id;
    std::vector<std::string> args_json;
    uint32_t dropped_since_last;
};

struct ReadyEvent {
    bool ready;
    std::string reason;
};

Json encode_hello(const Hello&);
Hello decode_hello(const Json&);

Json encode_eval_request(const EvalRequest&);
EvalRequest decode_eval_request(const Json&);

Json encode_eval_response(const EvalResponse&);
EvalResponse decode_eval_response(const Json&);

Json encode_hook_install(const HookInstallRequest&);
HookInstallRequest decode_hook_install(const Json&);

Json encode_hook_hit(const HookHitEvent&);
HookHitEvent decode_hook_hit(const Json&);

Json encode_ready(const ReadyEvent&);
ReadyEvent decode_ready(const Json&);

std::vector<uint8_t> frame_pack(const Json&);
std::optional<Json> frame_try_unpack(std::vector<uint8_t>& buf);

} // namespace positron::wire
