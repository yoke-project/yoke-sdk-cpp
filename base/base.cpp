// The base: what the libraries of this project share and nothing else. A concept that exists on one
// contract only does not live here: a candidate is in the base only if every library of the project
// would otherwise implement it.
#include "wire.hpp"

#include <chrono>
#include <utility>

#include "yoke/plugin/v1/contract.pb.h"

namespace yoke {

Refusal::Refusal(std::string code, const std::string &message, std::string stage, std::string item,
                 Subject subject)
    : std::runtime_error(stage.empty() ? code + ": " + message : code + " at " + stage + ": " + message),
      code_(std::move(code)), stage_(std::move(stage)), item_(std::move(item)), subject_(std::move(subject)) {
}

int plugin_contract() { return pluginv1::CONTRACT_VERSION; }

Env environment(const Getenv &getenv) {
    std::string missing;
    for (const char *name : {"YOKE_UNIT", "YOKE_SOCKET", "YOKE_BIND"}) {
        const char *value = getenv(name);
        if (value == nullptr || *value == '\0') {
            missing += (missing.empty() ? "" : ", ") + std::string(name);
        }
    }
    if (!missing.empty()) {
        throw std::runtime_error("the environment does not carry " + missing);
    }
    auto read = [&getenv](const char *name) {
        const char *value = getenv(name);
        return std::string(value != nullptr ? value : "");
    };
    return Env{read("YOKE_PLUGIN"), read("YOKE_UNIT"), read("YOKE_SOCKET"), read("YOKE_BIND"),
               read("YOKE_TOKEN")};
}

std::shared_ptr<grpc::Channel> dial(const std::string &path) {
    return grpc::CreateChannel("unix:" + path, grpc::InsecureChannelCredentials());
}

Refusal refusal_of(const pluginv1::Error &error) {
    return Refusal(error.code(), error.message(), {},
                   error.has_withheld() ? error.withheld().item() : std::string());
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

pluginv1::Envelope &Envelopes::seal(pluginv1::Envelope &e) {
    std::uint64_t n;
    {
        std::lock_guard<std::mutex> lock(mu_);
        n = ++next_;
    }
    e.set_message_id("u-" + std::to_string(n));
    e.set_session_id(session_);
    e.set_sent_at_unix_nano(now_ns());
    return e;
}

pluginv1::Envelope &Envelopes::answer(const std::string &to, pluginv1::Envelope &e) {
    seal(e);
    e.set_correlation_id(to);
    return e;
}

} // namespace yoke
