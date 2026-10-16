// The C++ interface library's harness: the thinnest translation between the suite's directives and the
// library. Launched by the suite against an instance, it attaches to the channel a directive names at the
// address the library computes, turns each further directive into one call of the library, reports what
// the library answered in the suite's vocabulary — a refusal as its code and what it names, never the
// library's words — and what the standing subscription or a subscription delivers as observations, in
// order. It judges nothing, confirms only when told to, and speaks no wire of its own.
#ifndef YOKE_INTERFACE_HARNESS_HPP
#define YOKE_INTERFACE_HARNESS_HPP

#include "yoke/interface.hpp"

namespace interface_harness {

// Runs the harness against the control socket getenv names as CONFORMANCE_SOCKET, and the instance it
// names as CONFORMANCE_INSTANCE, until it is told to finish; its exit status.
int serve(const yoke::Getenv &getenv);

} // namespace interface_harness

#endif
