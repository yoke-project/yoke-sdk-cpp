// What the libraries of this project share and nothing else: the addresses a party computes from its
// environment, a refusal as C++ carries a failure, and the contract version each library states.
//
// A failure is thrown. A refusal is a yoke::Refusal, carrying a code from the one namespace; a failure
// that is not one is a std::runtime_error carrying a message alone.
#ifndef YOKE_BASE_HPP
#define YOKE_BASE_HPP

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace yoke {

// What this project's libraries say they are: at admission, and in a harness's hello.
inline constexpr const char *sdk_line = "yoke-sdk-cpp 0.4.0";

// What a refusal names: a kind, an identity, and a unit's life where one is named.
struct Subject {
    std::string kind;
    std::string identity;
    std::uint64_t incarnation = 0;
};

// A refusal as it travels: a code from the one namespace, a message for a person, and the stage of
// admission where there is one.
class Refusal : public std::runtime_error {
  public:
    Refusal(std::string code, const std::string &message, std::string stage = {}, std::string item = {},
            Subject subject = {});

    const std::string &code() const noexcept { return code_; }
    const std::string &stage() const noexcept { return stage_; }
    // The item withheld or undeclared, where it names one.
    const std::string &item() const noexcept { return item_; }
    const Subject &subject() const noexcept { return subject_; }

  private:
    std::string code_, stage_, item_;
    Subject subject_;
};

// Reads one variable of the environment a party is handed; nullptr when it is not set.
using Getenv = std::function<const char *(const char *name)>;

// What a party is handed, and all it may assume.
struct Env {
    std::string plugin; // the plugin this unit is a copy of
    std::string unit;   // this unit's identity
    std::string socket; // the instance's plugin channel
    std::string bind;   // the path this unit is expected to bind
    std::string token;  // the bootstrap token
};

// Reads the reserved variables. A missing one is a failure naming it; no path is assumed.
Env environment(const Getenv &getenv);

// The version of the plugin contract the definitions carry, which the plugin library declares.
int plugin_contract();

} // namespace yoke

#endif
