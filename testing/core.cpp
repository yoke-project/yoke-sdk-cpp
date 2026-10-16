#include "core.hpp"

#include <chrono>
#include <thread>

#include <google/protobuf/any.pb.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

grpc::Status Core::Attach(grpc::ServerContext *context,
                          grpc::ServerReaderWriter<iv1::CoreFrame, iv1::ClientFrame> *stream) {
    {
        std::lock_guard<std::mutex> hold(mu_);
        attachments_++;
    }
    if (refuse) {
        return refused(grpc::StatusCode::FAILED_PRECONDITION, *refuse);
    }
    std::mutex sending;
    Send send = [&](const iv1::CoreFrame &f) {
        std::lock_guard<std::mutex> hold(sending);
        stream->Write(f);
    };
    iv1::CoreFrame open;
    *open.mutable_opening() = opening;
    send(open);
    for (const auto &f : standing) {
        send(f);
    }
    std::vector<std::thread> answering;
    iv1::ClientFrame f;
    bool ended_early = false;
    while (stream->Read(&f)) {
        if (f.has_cancel()) {
            std::lock_guard<std::mutex> hold(mu_);
            cancelled_.push_back(f.call());
            continue;
        }
        std::size_t n;
        {
            std::lock_guard<std::mutex> hold(mu_);
            requests_.push_back(f.request());
            n = requests_.size();
        }
        if (answer) {
            answering.emplace_back(
                [this, call = f.call(), r = f.request(), &send] { answer(call, r, send); });
        }
        if (end_after != 0 && static_cast<int>(n) >= end_after) {
            ended_early = true;
            break;
        }
    }
    for (auto &t : answering) {
        t.join();
    }
    if (ended_early) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return grpc::Status::OK;
    }
    std::lock_guard<std::mutex> hold(mu_);
    endings_.push_back(context->IsCancelled() ? "gone" : "closed");
    return grpc::Status::OK;
}

int Core::attachments() {
    std::lock_guard<std::mutex> hold(mu_);
    return attachments_;
}

std::vector<iv1::Request> Core::requests() {
    std::lock_guard<std::mutex> hold(mu_);
    return requests_;
}

std::vector<std::string> Core::cancelled() {
    std::lock_guard<std::mutex> hold(mu_);
    return cancelled_;
}

std::vector<std::string> Core::endings() {
    std::lock_guard<std::mutex> hold(mu_);
    return endings_;
}

std::unique_ptr<grpc::Server> serve(Core &core, const std::string &path) {
    grpc::ServerBuilder b;
    b.AddListeningPort("unix:" + path, grpc::InsecureServerCredentials());
    b.RegisterService(&core);
    return b.BuildAndStart();
}

grpc::Status refused(grpc::StatusCode code, const iv1::Refusal &refusal) {
    google::protobuf::Any any;
    any.PackFrom(refusal);
    std::string details;
    {
        google::protobuf::io::StringOutputStream raw(&details);
        google::protobuf::io::CodedOutputStream out(&raw);
        // google.rpc.Status: 1 code, 2 message, 3 details.
        out.WriteTag((1 << 3) | 0);
        out.WriteVarint32(static_cast<std::uint32_t>(code));
        out.WriteTag((2 << 3) | 2);
        out.WriteVarint32(static_cast<std::uint32_t>(refusal.message().size()));
        out.WriteString(refusal.message());
        std::string packed = any.SerializeAsString();
        out.WriteTag((3 << 3) | 2);
        out.WriteVarint32(static_cast<std::uint32_t>(packed.size()));
        out.WriteString(packed);
    }
    return grpc::Status(code, refusal.message(), details);
}

iv1::Opening opening_at(unsigned version) {
    iv1::Opening o;
    o.set_version(version);
    o.set_subscription("standing");
    o.mutable_picture()->set_at(4);
    o.mutable_picture()->add_records()->mutable_unit()->mutable_declared()->set_identity("acquire");
    return o;
}

iv1::CoreFrame answered(const std::string &call, const iv1::Response &response) {
    iv1::CoreFrame f;
    f.set_call(call);
    *f.mutable_answer() = response;
    return f;
}

iv1::CoreFrame event_at(std::uint64_t sequence) {
    iv1::CoreFrame f;
    f.set_call("standing");
    f.mutable_event()->set_seq(sequence);
    return f;
}
