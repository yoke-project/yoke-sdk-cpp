// The cases of a test program, each run in a process of its own and reported by the identifier its
// marker names, in the form the record writer reads: `pass  <id>`, or `FAIL  <id> — <why>`.
#ifndef YOKE_CHECK_HPP
#define YOKE_CHECK_HPP

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace check {

// A case that failed, and why.
struct Failed : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Case {
    const char *id;
    void (*run)();
};

// Runs every case, each in a child process with a bound on its time; 0 when all passed.
int run(const std::vector<Case> &cases);

// A directory of the case's own, removed when the case ends.
const std::string &dir();

// Waits ms milliseconds.
void sleep(int ms);

// Milliseconds on a monotonic clock.
long long ms();

// The whole of a file; empty when it cannot be read.
std::string read(const std::string &path);

template <typename... Parts> std::string say(const Parts &...parts) {
    std::ostringstream out;
    (out << ... << parts);
    return out.str();
}

} // namespace check

// Fails the case, saying why, unless cond holds.
#define REQUIRE(cond, ...)                                                                                   \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            throw check::Failed(check::say(__VA_ARGS__));                                                    \
        }                                                                                                    \
    } while (0)

#endif
