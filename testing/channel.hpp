// A plugin channel the cases stand in for the Core with: it answers a registration on the terms a case
// sets, keeps everything that arrives on the Session with the moment it arrived, and sends what a case
// hands it.
#ifndef YOKE_CHANNEL_HPP
#define YOKE_CHANNEL_HPP

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "yoke/base.hpp"
#include "yoke/plugin/v1/register.grpc.pb.h"
#include "yoke/plugin/v1/session.grpc.pb.h"

namespace pluginv1 = ::yoke::plugin::v1;

// How the channel answers a registration.
struct Terms {
    pluginv1::RegisterResponse::Outcome outcome = pluginv1::RegisterResponse::OUTCOME_ACCEPTED;
    pluginv1::Stage stage = pluginv1::STAGE_UNSPECIFIED;
    std::string code, message;
    std::string session = "s-1";
    pluginv1::Surface granted, withheld;
    int interval_ms = 0; // 0: no heartbeat terms
};

class Channel {
  public:
    // Listens in dir, at `plugin.sock`, and expects the unit to bind `unit.sock` beside it.
    explicit Channel(const std::string &dir, Terms terms = {});
    ~Channel();

    const std::string &socket() const { return socket_; }
    const std::string &bind() const { return bind_; }

    // A getenv for Unit::start: the five variables, pointing at this channel.
    yoke::Getenv getenv() const;

    // How many registrations, and how many Sessions, arrived.
    int registrations();
    int sessions();

    // The first registration that arrived; and whether the unit's socket was there when it did.
    std::optional<pluginv1::RegisterRequest> request();
    bool bound_at_registration();

    // How many envelopes the Session brought; the i-th, and when it arrived in check::ms() time.
    std::size_t count();
    pluginv1::Envelope arrival(std::size_t i, long long *at = nullptr);

    // Waits for an envelope whose payload is the case given, at index from or later; its index, or -1
    // if none arrived within timeout_ms.
    long await(pluginv1::Envelope::PayloadCase payload, std::size_t from, int timeout_ms);

    // Sends e to the unit on the Session, sealing it; its message identity.
    std::string send(pluginv1::Envelope e);

    // Ends the Session's stream from the channel's side.
    void end();

  private:
    class Registration;
    class Session;

    std::string socket_, bind_;
    Terms terms_;
    std::unique_ptr<Registration> registration_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<grpc::Server> server_;

    std::mutex mu_;
    std::condition_variable changed_;
    int registrations_ = 0, sessions_ = 0;
    std::optional<pluginv1::RegisterRequest> request_;
    bool bound_ = false;
    std::vector<pluginv1::Envelope> arrivals_;
    std::vector<long long> at_;

    // The open Session's stream, which a case writes to; written under write_mu_.
    std::mutex write_mu_;
    grpc::ServerReaderWriter<pluginv1::Envelope, pluginv1::Envelope> *stream_ = nullptr;
    grpc::ServerContext *context_ = nullptr;
    std::vector<pluginv1::Envelope> waiting_; // sent before the Session opened
    bool end_asked_ = false;
    unsigned long long next_ = 0;
};

#endif
