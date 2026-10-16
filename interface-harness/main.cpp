// The C++ interface harness, as the suite launches it.
#include <cstdlib>

#include "interface_harness.hpp"

int main() {
    return interface_harness::serve([](const char *name) { return std::getenv(name); });
}
