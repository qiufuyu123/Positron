#include <napi.h>

Napi::Value Hello(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), "world");
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("hello", Napi::Function::New(env, Hello));
    return exports;
}

NODE_API_MODULE(sample_addon, Init)
