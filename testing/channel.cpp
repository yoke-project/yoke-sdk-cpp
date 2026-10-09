#include "channel.hpp"

#include <chrono>
#include <cstring>

#include <sys/stat.h>

#include "check.hpp"

class Channel::Registration final : public pluginv1::Register::Service {
  public:
    explicit Registration(Channel &ch) : ch_(ch) {}

    grpc::Status Register(grpc::ServerContext *, const pluginv1::RegisterRequest *request,
                          pluginv1::RegisterResponse *response) override {
        struct stat st;
        bool bound = stat(ch_.bind_.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
        {
            std::lock_guard<std::mutex> lock(ch_.mu_);
            ch_.registrations_++;
            if (!ch_.request_) {
                ch_.request_ = *request;
                ch_.bound_ = bound;
            }
            ch_.changed_.notify_all();
        }
        const Terms &t = ch_.terms_;
        response->set_outcome(t.outcome);
        response->set_stage(t.stage);
        response->set_code(t.code);
        response->set_message(t.message);
        if (t.outcome != pluginv1::RegisterResponse::OUTCOME_REFUSED) {
            response->set_session_id(t.session);
            *response->mutable_granted() = t.granted;
            *response->mutable_withheld() = t.withheld;
            if (t.interval_ms > 0) {
                auto *interval = response->mutable_heartbeat()->mutable_interval();
                interval->set_seconds(t.interval_ms / 1000);
                interval->set_nanos((t.interval_ms % 1000) * 1000000);
                response->mutable_heartbeat()->set_tolerance(3);
            }
        }
        return grpc::Status::OK;
    }

  private:
    Channel &ch_;
};

class Channel::Session final : public pluginv1::Session::Service {
  public:
    explicit Session(Channel &ch) : ch_(ch) {}

    grpc::Status Open(grpc::ServerContext *context,
                      grpc::ServerReaderWriter<pluginv1::Envelope, pluginv1::Envelope> *stream) override {
        {
            std::lock_guard<std::mutex> lock(ch_.mu_);
            ch_.sessions_++;
            ch_.changed_.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(ch_.write_mu_);
            if (ch_.stream_ != nullptr) {
                return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "a Session is open");
            }
            ch_.stream_ = stream;
            ch_.context_ = context;
            for (const auto &e : ch_.waiting_) {
                stream->Write(e);
            }
            ch_.waiting_.clear();
            if (ch_.end_asked_) {
                context->TryCancel();
            }
        }
        pluginv1::Envelope e;
        while (stream->Read(&e)) {
            std::lock_guard<std::mutex> lock(ch_.mu_);
            ch_.arrivals_.push_back(e);
            ch_.at_.push_back(check::ms());
            ch_.changed_.notify_all();
        }
        std::lock_guard<std::mutex> lock(ch_.write_mu_);
        ch_.stream_ = nullptr;
        ch_.context_ = nullptr;
        return grpc::Status::OK;
    }

  private:
    Channel &ch_;
};

Channel::Channel(const std::string &dir, Terms terms)
    : socket_(dir + "/plugin.sock"), bind_(dir + "/unit.sock"), terms_(std::move(terms)),
      registration_(std::make_unique<Registration>(*this)), session_(std::make_unique<Session>(*this)) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("unix:" + socket_, grpc::InsecureServerCredentials());
    builder.RegisterService(registration_.get());
    builder.RegisterService(session_.get());
    server_ = builder.BuildAndStart();
    if (!server_) {
        throw std::runtime_error("the channel cannot listen at " + socket_);
    }
}

Channel::~Channel() {
    server_->Shutdown(std::chrono::system_clock::now());
    server_->Wait();
}

yoke::Getenv Channel::getenv() const {
    return [this](const char *name) -> const char * {
        if (std::strcmp(name, "YOKE_PLUGIN") == 0) {
            return "com.yoke.station.acquire";
        }
        if (std::strcmp(name, "YOKE_UNIT") == 0) {
            return "acquire";
        }
        if (std::strcmp(name, "YOKE_SOCKET") == 0) {
            return socket_.c_str();
        }
        if (std::strcmp(name, "YOKE_BIND") == 0) {
            return bind_.c_str();
        }
        if (std::strcmp(name, "YOKE_TOKEN") == 0) {
            return "t-0123";
        }
        return nullptr;
    };
}

int Channel::registrations() {
    std::lock_guard<std::mutex> lock(mu_);
    return registrations_;
}

int Channel::sessions() {
    std::lock_guard<std::mutex> lock(mu_);
    return sessions_;
}

std::optional<pluginv1::RegisterRequest> Channel::request() {
    std::lock_guard<std::mutex> lock(mu_);
    return request_;
}

bool Channel::bound_at_registration() {
    std::lock_guard<std::mutex> lock(mu_);
    return bound_;
}

std::size_t Channel::count() {
    std::lock_guard<std::mutex> lock(mu_);
    return arrivals_.size();
}

pluginv1::Envelope Channel::arrival(std::size_t i, long long *at) {
    std::lock_guard<std::mutex> lock(mu_);
    if (i >= arrivals_.size()) {
        return {};
    }
    if (at != nullptr) {
        *at = at_[i];
    }
    return arrivals_[i];
}

long Channel::await(pluginv1::Envelope::PayloadCase payload, std::size_t from, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(mu_);
    std::size_t i = from;
    for (;;) {
        for (; i < arrivals_.size(); i++) {
            if (arrivals_[i].payload_case() == payload) {
                return static_cast<long>(i);
            }
        }
        if (changed_.wait_until(lock, deadline) == std::cv_status::timeout) {
            for (; i < arrivals_.size(); i++) {
                if (arrivals_[i].payload_case() == payload) {
                    return static_cast<long>(i);
                }
            }
            return -1;
        }
    }
}

std::string Channel::send(pluginv1::Envelope e) {
    std::lock_guard<std::mutex> lock(write_mu_);
    std::string id = "c-" + std::to_string(++next_);
    e.set_message_id(id);
    e.set_session_id(terms_.session);
    e.set_sent_at_unix_nano(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
    if (stream_ != nullptr) {
        stream_->Write(e);
    } else {
        waiting_.push_back(e);
    }
    return id;
}

void Channel::end() {
    std::lock_guard<std::mutex> lock(write_mu_);
    end_asked_ = true;
    if (context_ != nullptr) {
        context_->TryCancel();
    }
}
