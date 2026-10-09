#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <string>

#include "check.hpp"
#include "wire.hpp"

namespace {

// The base's sources, which case 1 reads.
const char *const sources[] = {SOURCE_DIR "/base/base.cpp", SOURCE_DIR "/base/wire.hpp",
                               SOURCE_DIR "/include/yoke/base.hpp"};

// What belongs to one contract among the definitions, which the base may not refer to; a payload's
// case names the message it carries.
bool forbidden(const std::string &name) {
    static const char *const words[] = {"Register", "Session", "Surface", "Heartbeat", "Stage", "Control",
                                        "Query",    "Ack",     "Data",    "Health",    "Event"};
    for (const char *word : words) {
        if (name.rfind(word, 0) == 0 || name.rfind(std::string("Envelope::k") + word, 0) == 0) {
            return true;
        }
    }
    return false;
}

// std: yoke-sdk-cpp:the-base.01
void test_the_base_holds_nothing_of_one_contract() {
    const std::regex definition(R"((?:pluginv1|plugin::v1)::([A-Za-z_][A-Za-z0-9_:]*))");
    const std::regex include(R"(#include\s+["<]([^">]+)[">])");
    int referred = 0;
    for (const char *source : sources) {
        std::string text = check::read(source);
        REQUIRE(!text.empty(), source, " cannot be read");
        for (auto m = std::sregex_iterator(text.begin(), text.end(), definition); m != std::sregex_iterator();
             ++m) {
            std::string name = (*m)[1];
            REQUIRE(!forbidden(name), source, " refers to ", name, ", which belongs to one contract");
            referred++;
        }
        for (auto m = std::sregex_iterator(text.begin(), text.end(), include); m != std::sregex_iterator();
             ++m) {
            std::string header = (*m)[1];
            bool ours = header.rfind("yoke/", 0) == 0 && header != "yoke/base.hpp" &&
                        header.rfind("yoke/plugin/v1/", 0) != 0;
            REQUIRE(!ours, source, " includes ", header, ", a header of a library of this project");
        }
    }
    REQUIRE(referred > 0, "the base refers to nothing of the definitions, so nothing was read");
}

// std: yoke-sdk-cpp:the-base.02
void test_a_refusal_carries_its_code_and_envelopes_are_correlated() {
    yoke::pluginv1::Error error;
    error.set_code("session.correlation.unknown");
    error.set_message("nothing was sent with that identity");
    try {
        throw yoke::refusal_of(error);
    } catch (const yoke::Refusal &r) {
        REQUIRE(r.code() == "session.correlation.unknown", "the refusal's code is ", r.code());
        REQUIRE(std::string(r.what()).find(error.message()) != std::string::npos, "the refusal says ",
                r.what());
    }

    yoke::Envelopes envelopes("s-7");
    yoke::pluginv1::Envelope e[3];
    envelopes.seal(e[0]);
    envelopes.seal(e[1]);
    envelopes.answer(e[0].message_id(), e[2]);
    for (int i = 0; i < 3; i++) {
        REQUIRE(e[i].session_id() == "s-7", "envelope ", i, " carries the Session ", e[i].session_id());
        REQUIRE(!e[i].message_id().empty(), "envelope ", i, " has no message identity");
        REQUIRE(e[i].sent_at_unix_nano() > 0, "envelope ", i, " carries no clock");
        for (int j = 0; j < i; j++) {
            REQUIRE(e[i].message_id() != e[j].message_id(), "envelopes ", j, " and ", i, " share ",
                    e[i].message_id());
        }
    }
    REQUIRE(e[2].correlation_id() == e[0].message_id(), "the answer is correlated to ",
            e[2].correlation_id());
    REQUIRE(e[2].correlation_id() != e[2].message_id(), "the answer is correlated to itself");
}

// std: yoke-sdk-cpp:the-base.03
void test_the_addresses_come_from_the_environment() {
    const std::map<std::string, std::string> five = {{"YOKE_PLUGIN", "com.yoke.station.acquire"},
                                                     {"YOKE_UNIT", "acquire"},
                                                     {"YOKE_SOCKET", "/run/yoke/i/plugin.sock"},
                                                     {"YOKE_BIND", "/run/yoke/i/units/acquire.sock"},
                                                     {"YOKE_TOKEN", "t-0123"}};
    auto from = [&](const char *missing) {
        return [&five, missing](const char *name) -> const char * {
            if (missing != nullptr && std::strcmp(name, missing) == 0) {
                return nullptr;
            }
            auto found = five.find(name);
            return found == five.end() ? nullptr : found->second.c_str();
        };
    };
    yoke::Env env = yoke::environment(from(nullptr));
    REQUIRE(env.plugin == "com.yoke.station.acquire", "the plugin is ", env.plugin);
    REQUIRE(env.unit == "acquire", "the unit is ", env.unit);
    REQUIRE(env.socket == "/run/yoke/i/plugin.sock", "the channel is ", env.socket);
    REQUIRE(env.bind == "/run/yoke/i/units/acquire.sock", "the path to bind is ", env.bind);
    REQUIRE(env.token == "t-0123", "the token is ", env.token);
    try {
        yoke::Env without = yoke::environment(from("YOKE_SOCKET"));
        REQUIRE(false, "an environment without YOKE_SOCKET was read, giving the channel ", without.socket);
    } catch (const std::runtime_error &e) {
        REQUIRE(dynamic_cast<const check::Failed *>(&e) == nullptr, e.what());
        REQUIRE(std::string(e.what()).find("YOKE_SOCKET") != std::string::npos,
                "the failure does not name YOKE_SOCKET: ", e.what());
    }
}

} // namespace

int main() {
    return check::run({
        {"yoke-sdk-cpp:the-base.01", test_the_base_holds_nothing_of_one_contract},
        {"yoke-sdk-cpp:the-base.02", test_a_refusal_carries_its_code_and_envelopes_are_correlated},
        {"yoke-sdk-cpp:the-base.03", test_the_addresses_come_from_the_environment},
    });
}
