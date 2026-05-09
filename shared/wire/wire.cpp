#include "wire.h"
#include <cstring>
#include <stdexcept>

namespace positron::wire {

Json encode_hello(const Hello& h) {
    return Json{
        {"kind", "hello"},
        {"electron_version", h.electron_version},
        {"target_type", h.target_type},
        {"napi_symbols_present", h.napi_symbols_present},
        {"pid", h.pid},
    };
}
Hello decode_hello(const Json& j) {
    return Hello{
        j.value("electron_version", std::string{}),
        j.value("target_type", std::string{}),
        j.value("napi_symbols_present", std::vector<std::string>{}),
        j.value("pid", 0u),
    };
}

Json encode_eval_request(const EvalRequest& r) {
    Json j{{"kind","eval"},{"id",r.id},{"code",r.code},{"world",r.world}};
    if (r.world_index) j["world_index"] = *r.world_index;
    return j;
}
EvalRequest decode_eval_request(const Json& j) {
    EvalRequest r;
    r.id = j.at("id").get<uint64_t>();
    r.code = j.at("code").get<std::string>();
    r.world = j.value("world", std::string{"auto"});
    if (j.contains("world_index")) r.world_index = j.at("world_index").get<int32_t>();
    return r;
}

Json encode_eval_response(const EvalResponse& r) {
    Json j{{"kind","eval.response"},{"id",r.id},{"ok",r.ok}};
    if (r.result) j["result"] = {{"type_tag", r.result->type_tag}, {"json", r.result->json}};
    if (r.error)  j["error"]  = {{"message", r.error->message}, {"stack", r.error->stack}};
    return j;
}
EvalResponse decode_eval_response(const Json& j) {
    EvalResponse r;
    r.id = j.at("id").get<uint64_t>();
    r.ok = j.at("ok").get<bool>();
    if (j.contains("result")) r.result = EvalResult{
        j["result"].value("type_tag", std::string{}),
        j["result"].value("json", std::string{})
    };
    if (j.contains("error")) r.error = EvalError{
        j["error"].value("message", std::string{}),
        j["error"].value("stack", std::string{})
    };
    return r;
}

Json encode_hook_install(const HookInstallRequest& r) {
    return Json{
        {"kind","hook.install"},
        {"id",r.id},
        {"symbol",r.symbol},
        {"script",r.script}
    };
}
HookInstallRequest decode_hook_install(const Json& j) {
    return HookInstallRequest{
        j.at("id").get<uint64_t>(),
        j.at("symbol").get<std::string>(),
        j.value("script", std::string{})
    };
}

Json encode_hook_hit(const HookHitEvent& e) {
    return Json{
        {"kind","hook.hit"},
        {"hook_id",e.hook_id},
        {"args_json",e.args_json},
        {"dropped_since_last", e.dropped_since_last}
    };
}
HookHitEvent decode_hook_hit(const Json& j) {
    return HookHitEvent{
        j.at("hook_id").get<uint64_t>(),
        j.value("args_json", std::vector<std::string>{}),
        j.value("dropped_since_last", 0u)
    };
}

Json encode_ready(const ReadyEvent& e) {
    return Json{{"kind","ready"},{"ready",e.ready},{"reason",e.reason}};
}
ReadyEvent decode_ready(const Json& j) {
    return ReadyEvent{ j.at("ready").get<bool>(), j.value("reason", std::string{}) };
}

std::vector<uint8_t> frame_pack(const Json& j) {
    std::string s = j.dump();
    if (s.size() > kMaxFrameBytes) throw std::runtime_error("frame too large");
    std::vector<uint8_t> out(4 + s.size());
    uint32_t len = static_cast<uint32_t>(s.size());
    std::memcpy(out.data(), &len, 4);
    std::memcpy(out.data() + 4, s.data(), s.size());
    return out;
}

std::optional<Json> frame_try_unpack(std::vector<uint8_t>& buf) {
    if (buf.size() < 4) return std::nullopt;
    uint32_t len;
    std::memcpy(&len, buf.data(), 4);
    if (len > kMaxFrameBytes) throw std::runtime_error("frame too large");
    if (buf.size() < 4 + len) return std::nullopt;
    Json j = Json::parse(buf.begin() + 4, buf.begin() + 4 + len);
    buf.erase(buf.begin(), buf.begin() + 4 + len);
    return j;
}

} // namespace positron::wire
