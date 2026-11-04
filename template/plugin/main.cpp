// @YOKE_PROGRAM@: a Yoke Plugin. Its Manifest is kept beside this file, as @YOKE_ID@.yaml, and is what
// `@YOKE_PROGRAM@ --manifest` prints; the continuous integration of this project compares the two.
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <yoke/plugin.hpp>

using namespace yoke::plugin;

// What this Plugin says about itself: its identity, and whatever it publishes and answers.
Declaration declaration() { return {.id = "@YOKE_ID@"}; }

static std::atomic<bool> stopping;

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--manifest") {
        std::cout << declaration().manifest();
        return 0;
    }
    std::signal(SIGTERM, [](int) { stopping = true; });
    try {
        auto unit = Unit::start(declaration());
        unit->health(0, "ready");
        while (!stopping && !unit->done()) {
            unit->next(std::chrono::milliseconds(200));
        }
        if (stopping) {
            unit->close();
            while (unit->next()) {
            }
        }
    } catch (const std::exception &err) {
        std::cerr << "@YOKE_PROGRAM@: " << err.what() << '\n';
        return 1;
    }
}
