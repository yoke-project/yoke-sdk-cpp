// The library an interface client is written with: the interface contract, on its typed projection,
// over the base.
//
// A client attaches to a channel and is given its opening: what the channel may address, the standing
// subscription, and the contract's version. Every operation of the contract's union is one member, all
// on the one connection, each answered on its own; a refusal is the base's yoke::Refusal, carrying its
// code and its detail. Nothing is retried and nothing reconnects, and nothing is sent the author did not
// ask for: a confirmation is the author's statement of what it has processed, so the library never makes
// one.
//
// A channel at a routable address is reached over TLS 1.3 against the roots the caller supplies, and
// establishes its caller before anything else: the credential is the attachment's first frame, and the
// session token its answer carries is the caller's to present to a later attachment.
//
// What the contract carries — records, snapshots, events, filters, acknowledgements — is surfaced as the
// contract's own messages, generated from its definitions and installed beside this header.
//
// The library runs one thread of its own per attachment, reading what the Core sends, and one more on a
// routable address, carrying the TLS. Every member may be called from any thread.
#ifndef YOKE_INTERFACE_HPP
#define YOKE_INTERFACE_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "yoke/base.hpp"
#include "yoke/interface/v1/interface.pb.h"

namespace yoke::iface {

// The contract's messages.
namespace v1 = ::yoke::interface::v1;

// The version of the interface contract this library covers, stated on every request.
int contract();

// A channel's address in the service form, under its runtime directory. A channel name that could choose
// where the socket is is refused.
std::string service(const std::string &runtime_dir, const std::string &channel);

// A channel's address in an application instance, derived from the instance's name under the account's
// runtime directory.
std::string application(const std::string &instance, const std::string &channel, const Getenv &getenv);

// The address of the channel a managed interface was launched for, which the Core hands it.
std::string managed(const Getenv &getenv);

// The operation a request carries, as the contract names it.
std::string name(const v1::Request &request);

// A refusal of a suspended channel: the base's refusal, with the grade the channel is suspended in and
// the channel it is suspended in favour of.
class Suspended : public Refusal {
  public:
    Suspended(const Refusal &refusal, std::string grade, std::string by)
        : Refusal(refusal), grade_(std::move(grade)), by_(std::move(by)) {}
    const std::string &grade() const noexcept { return grade_; }
    const std::string &by() const noexcept { return by_; }

  private:
    std::string grade_, by_;
};

// What a call throws once the attachment has ended.
class Ended : public std::runtime_error {
  public:
    Ended() : std::runtime_error("the attachment ended") {}
};

// How an attachment reaches its channel, and what it presents there.
struct Options {
    // TLS 1.3 and nothing below it, verifying the Core's chain against roots, in PEM, and no others;
    // server_name is the name the certificate must carry, the address's host where it is empty. No client
    // certificate is offered: the caller is established by a credential, above the transport.
    static Options tls(std::string roots, std::string server_name = {});

    // Presents an account and its secret, for the host to judge, as the attachment's first frame.
    Options &password(std::string account, std::string secret);

    // Presents a session token an earlier authentication on the same channel issued, as the attachment's
    // first frame.
    Options &token(std::string token);

    bool use_tls = false;
    std::string roots, server_name;
    std::optional<v1::Authenticate> credential;
};

// How long a call waits for its answer before it gives up on it, and says so.
inline constexpr std::chrono::milliseconds answer_within{30000};

class Attachment;

// A subscription, delivered in order: its snapshot, then events and overflows, until its caller ends it,
// the Core completes it or the attachment ends. Nothing reconnects it.
class Subscription {
  public:
    ~Subscription();
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;

    // What a confirmation names.
    const std::string &id() const;

    // The next delivery, waiting for it; nothing once the Core completed the subscription. Throws Ended
    // once the attachment ended, and the refusal the Core ended it with.
    std::optional<v1::Subscribed> next();

    // Ends the subscription on the wire. The standing subscription is the attachment's, and ends with it.
    void close();

    struct State;
    explicit Subscription(std::shared_ptr<State> state);

  private:
    std::shared_ptr<State> state_;
};

// One message of a stream as a delivery carries it: its sequence, the producer's clock, and the payload,
// opaque.
struct Frame {
    std::uint64_t sequence = 0;
    std::uint64_t sent_at = 0;
    std::string payload;
};

// A copy of a stream delivered to this client, read where the answer said it arrives.
class Delivery {
  public:
    ~Delivery();
    Delivery(const Delivery &) = delete;
    Delivery &operator=(const Delivery &) = delete;

    const std::string &id() const;
    // Whether the stream was flowing when the subscription was answered.
    bool flowing() const;

    // The next frame, in order; nothing once the delivery ended. Throws Ended once the attachment did.
    std::optional<Frame> next();

    // Ends the delivery: the Core is told, and what was opened for it is let go.
    void release(std::chrono::milliseconds wait = answer_within);

    struct State;
    explicit Delivery(std::shared_ptr<State> state);

  private:
    std::shared_ptr<State> state_;
};

// A client attached to a channel.
class Attachment {
  public:
    // Attaches to the channel at address — a path is a local socket, anything else `host:port` — and
    // waits for its opening at most wait. A Core that speaks another version of the contract is refused
    // before any operation, and the connection let go. A credential among the options is sent before
    // anything else, and attach waits for its answer too.
    static std::unique_ptr<Attachment> attach(const std::string &address, const Options &options = {},
                                              std::chrono::milliseconds wait = answer_within);

    // Closes the attachment, if it was not closed.
    ~Attachment();
    Attachment(const Attachment &) = delete;
    Attachment &operator=(const Attachment &) = delete;

    // What the channel was given when it attached.
    const v1::Snapshot &picture() const;
    // The subscription it holds from then on; nullptr where the opening named none.
    Subscription *standing() const;
    // The contract's version the Core speaks.
    int version() const;
    // The answer to the credential the attachment presented; nullptr where it presented none.
    const v1::Authenticated *established() const;

    // Presents an account and its secret, on a channel whose class takes a credential.
    v1::Authenticated authenticate(std::string_view account, std::string_view secret,
                                   std::chrono::milliseconds wait = answer_within);
    // Presents a token an earlier authentication issued.
    v1::Authenticated authenticate_token(std::string_view token,
                                         std::chrono::milliseconds wait = answer_within);
    // The records of a kind the channel sees, or of the one subject an identity names.
    std::vector<v1::Record> read(std::string_view kind, std::string_view identity = {},
                                 std::chrono::milliseconds wait = answer_within);
    // Opens a subscription narrowed by a filter, and returns once its snapshot or its refusal arrives.
    std::unique_ptr<Subscription> subscribe(const v1::Filter &filter,
                                            std::chrono::milliseconds wait = answer_within);
    // States that the author has processed a subscription up to a sequence. Only the author knows that, so
    // the library never confirms on its own.
    void confirm(std::string_view subscription, std::uint64_t sequence,
                 std::chrono::milliseconds wait = answer_within);
    // Sends a command to a unit, its payload opaque, and returns how the unit acknowledged it.
    v1::Acknowledged command(std::string_view unit, std::string_view type, std::string_view payload = {},
                             std::chrono::milliseconds wait = answer_within);
    // Asks a unit a question, its payload opaque, and returns the answer's.
    std::string ask(std::string_view unit, std::string_view type, std::string_view payload = {},
                    std::chrono::milliseconds wait = answer_within);
    // Asks for a unit's stream to flow, for everyone.
    v1::Acknowledged start_stream(std::string_view unit, std::string_view stream,
                                  std::chrono::milliseconds wait = answer_within);
    // Asks for a unit's stream to stop flowing.
    v1::Acknowledged stop_stream(std::string_view unit, std::string_view stream,
                                 std::chrono::milliseconds wait = answer_within);
    // Asks for a copy of a unit's stream, and opens it where the answer says it arrives: a per-subscriber
    // socket the Core listens on, or the attachment's own connection.
    std::unique_ptr<Delivery> subscribe_stream(std::string_view unit, std::string_view stream,
                                               std::chrono::milliseconds wait = answer_within);
    // Takes control back for a channel bound as a local socket.
    v1::Reclaimed reclaim(std::chrono::milliseconds wait = answer_within);

    // Ends the attachment in order: it tells the Core the client is done, so the Core reads a client that
    // closed rather than one that went away, and lets the connection go once the Core has ended it.
    void close();

    struct State;

  private:
    explicit Attachment(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace yoke::iface

#endif
