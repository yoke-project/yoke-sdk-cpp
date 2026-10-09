// The base's half that is not an author's: connecting to a socket, the envelope and its correlation,
// and a refusal from an error envelope. Shared by this project's libraries; never installed.
#ifndef YOKE_WIRE_HPP
#define YOKE_WIRE_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <grpcpp/grpcpp.h>

#include "yoke/base.hpp"
#include "yoke/plugin/v1/session.pb.h"

namespace yoke {

namespace pluginv1 = ::yoke::plugin::v1;

// Connects to a Unix socket.
std::shared_ptr<grpc::Channel> dial(const std::string &path);

// The refusal an error envelope carries.
Refusal refusal_of(const pluginv1::Error &error);

// The sender's clock, in nanoseconds since the epoch.
std::int64_t now_ns();

// Fills the header of every envelope one party sends in one Session: a message identity no other of
// its envelopes has, the Session's identity, and the sender's clock.
class Envelopes {
  public:
    explicit Envelopes(std::string session) : session_(std::move(session)) {}

    // Fills e's header.
    pluginv1::Envelope &seal(pluginv1::Envelope &e);

    // Fills e's header as an answer to the message identified by to.
    pluginv1::Envelope &answer(const std::string &to, pluginv1::Envelope &e);

  private:
    std::string session_;
    std::mutex mu_;
    std::uint64_t next_ = 0;
};

} // namespace yoke

#endif
