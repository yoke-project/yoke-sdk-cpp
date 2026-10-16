// The interface library against a published Core: run by ci/routable.sh, which starts the Core with a
// routable channel and says where it is — YOKE_L3_ADDRESS, and the chain it presents in YOKE_L3_ROOTS.
#include <cstdlib>
#include <string>

#include "check.hpp"
#include "yoke/interface.hpp"

using namespace std::chrono_literals;
namespace yi = yoke::iface;

namespace {

std::string required(const char *name) {
    const char *value = std::getenv(name);
    REQUIRE(value != nullptr && *value != '\0', name, " is not set: ci/routable.sh runs this against a Core");
    return value;
}

// std: yoke-sdk-cpp:across-a-routable-address.03
void test_the_published_cores_routable_channel_is_attached_and_signed_in() {
    std::string address = required("YOKE_L3_ADDRESS"), roots = check::read(required("YOKE_L3_ROOTS"));
    REQUIRE(!roots.empty(), "the chain the Core presents cannot be read");

    auto first = yi::Attachment::attach(address, yi::Options::tls(roots).password("alice", "hunter2x"), 10s);
    const auto *e = first->established();
    REQUIRE(first->version() == yi::contract() && first->standing() != nullptr,
            "the first attachment surfaced version ", first->version(), " and ",
            first->standing() ? "a" : "no", " standing subscription");
    REQUIRE(e != nullptr && e->account() == "alice" && !e->token().empty() && e->has_expires(),
            "the first attachment established ", e ? e->ShortDebugString() : std::string("nothing"));
    std::string token = e->token();
    first->close();

    auto second = yi::Attachment::attach(address, yi::Options::tls(roots).token(token), 10s);
    const auto *again = second->established();
    REQUIRE(second->version() == yi::contract() && second->standing() != nullptr,
            "the second attachment did not surface the opening");
    REQUIRE(again != nullptr && again->account() == "alice", "the token established ",
            again ? again->ShortDebugString() : std::string("nothing"));
    second->close();

    std::string code;
    try {
        yi::Attachment::attach(address, yi::Options::tls(roots).password("alice", "wrong"), 10s);
    } catch (const yoke::Refusal &r) {
        code = r.code();
    } catch (const std::exception &ex) {
        code = std::string("not a refusal: ") + ex.what();
    }
    REQUIRE(code == "auth.invalid", "another secret was answered '", code, "'");
}

} // namespace

int main() {
    return check::run({
        {"yoke-sdk-cpp:across-a-routable-address.03",
         test_the_published_cores_routable_channel_is_attached_and_signed_in},
    });
}
