// The C++ plugin harness, as the suite and the Core launch it.
#include <cstdlib>

#include "harness.hpp"

int main() {
    return harness::serve([](const char *name) { return std::getenv(name); });
}
