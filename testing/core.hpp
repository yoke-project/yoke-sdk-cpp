// A Core's interface surface the cases stand in for the Core with: it opens each attachment as a case
// sets, answers each request as the case scripts it, and records what it was sent and how each
// attachment ended.
#ifndef YOKE_TESTING_CORE_HPP
#define YOKE_TESTING_CORE_HPP

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "yoke/interface/v1/interface.grpc.pb.h"

namespace iv1 = ::yoke::interface::v1;

// Sends one frame on the attachment being answered.
using Send = std::function<void(const iv1::CoreFrame &)>;

class Core : public iv1::Interface::Service {
  public:
    iv1::Opening opening;
    // Refuses every attachment with this, as a status carrying it, when set.
    std::optional<iv1::Refusal> refuse;
    // Answers each request, on a thread of its own; nothing is answered when it is not set.
    std::function<void(const std::string &call, const iv1::Request &, const Send &)> answer;
    // Sent after the opening.
    std::vector<iv1::CoreFrame> standing;
    // Ends the attachment after this many requests, when not zero.
    int end_after = 0;

    grpc::Status Attach(grpc::ServerContext *context,
                        grpc::ServerReaderWriter<iv1::CoreFrame, iv1::ClientFrame> *stream) override;

    int attachments();
    std::vector<iv1::Request> requests();
    std::vector<std::string> cancelled();
    // How each attachment's requests ended: "closed" for a client that said it was done, "gone" for one
    // that went away.
    std::vector<std::string> endings();

  private:
    std::mutex mu_;
    int attachments_ = 0;
    std::vector<iv1::Request> requests_;
    std::vector<std::string> cancelled_, endings_;
};

// Serves core on a local socket at path.
std::unique_ptr<grpc::Server> serve(Core &core, const std::string &path);

// A refusal as a Core carries it on a status: in a google.rpc.Status's details.
grpc::Status refused(grpc::StatusCode code, const iv1::Refusal &refusal);

// An opening at version, with the standing subscription `standing`, the sequence 4 and one unit's record,
// `acquire`.
iv1::Opening opening_at(unsigned version);

// A frame answering call.
iv1::CoreFrame answered(const std::string &call, const iv1::Response &response);

// An event at sequence on the standing subscription.
iv1::CoreFrame event_at(std::uint64_t sequence);

#endif
