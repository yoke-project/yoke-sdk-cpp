// The plugin library. One thread reads the Session and turns what arrives into events; a second beats
// and keeps the deadline of a close. gRPC allows one write at a time beside a read, so every write goes
// under one lock.
#include "yoke/plugin.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "wire.hpp"
#include "yoke/plugin/v1/register.grpc.pb.h"
#include "yoke/plugin/v1/session.grpc.pb.h"

namespace yoke::plugin {

namespace {

using Clock = std::chrono::steady_clock;

// How long a close waits for the Core to end the stream before the departure is one anyway.
constexpr auto close_grace = std::chrono::seconds(2);

// ---- the Manifest ----

std::string quoted(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20 || c == 0x7f) {
            char escaped[5];
            std::snprintf(escaped, sizeof escaped, "\\x%02x", c);
            out += escaped;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

void ids(std::ostringstream &out, const char *key, const std::vector<std::string> &list) {
    if (list.empty()) {
        return;
    }
    out << key << ":\n";
    for (const auto &id : list) {
        out << "  - id: " << quoted(id) << "\n";
    }
}

const char *governed(Governs kind) {
    switch (kind) {
    case Governs::stream:
        return "stream";
    case Governs::command:
        return "command";
    case Governs::query:
        return "query";
    case Governs::occurrence:
        return "occurrence";
    case Governs::surface:
        return "surface";
    }
    return "";
}

// An enumeration's value as its name says it, without its prefix: `STAGE_UNIT_CONFLICT`, `unit conflict`.
std::string spoken(std::string name, const std::string &prefix) {
    if (name.rfind(prefix, 0) == 0) {
        name = name.substr(prefix.size());
    }
    if (name.empty()) {
        return "unspecified";
    }
    for (char &c : name) {
        c = c == '_' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

Scope scope_of(const pluginv1::Surface &s) {
    auto list = [](const auto &repeated) {
        return std::vector<std::string>(repeated.begin(), repeated.end());
    };
    return Scope{list(s.capabilities()), list(s.streams()), list(s.commands()), list(s.queries()),
                 list(s.occurrences())};
}

// Binds the unit's own socket.
int bind_unit(const std::string &path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        throw std::runtime_error("the unit's socket " + path + " cannot be bound: the path is too long");
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 ||
        ::listen(fd, 16) != 0) {
        int code = errno;
        if (fd >= 0) {
            ::close(fd);
        }
        throw std::runtime_error("the unit's socket " + path + " cannot be bound: " + std::strerror(code));
    }
    return fd;
}

// One activated stream: the library's connection to its transport, and the next sequence.
struct Flow {
    int fd = -1;
    bool framed = false;
    std::mutex mu;
    std::uint64_t next = 0;

    ~Flow() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

// Reaches the transport an activation names, as the Core created it; the reason when it cannot.
std::shared_ptr<Flow> connect_flow(const pluginv1::Control::Activate &a, std::string *why) {
    int type;
    switch (a.transport()) {
    case pluginv1::Control::Activate::TRANSPORT_ORDERED:
        type = SOCK_SEQPACKET;
        break;
    case pluginv1::Control::Activate::TRANSPORT_FRAMED:
        type = SOCK_DGRAM;
        break;
    default:
        *why = "the transport " +
               spoken(pluginv1::Control::Activate::Transport_Name(a.transport()), "TRANSPORT_") + " of " +
               a.stream() + " is not one this library reaches";
        return nullptr;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (a.address().size() >= sizeof addr.sun_path) {
        *why = "the transport of " + a.stream() + " at " + a.address() +
               " cannot be reached: the path is too long";
        return nullptr;
    }
    std::memcpy(addr.sun_path, a.address().c_str(), a.address().size() + 1);
    auto f = std::make_shared<Flow>();
    f->framed = type == SOCK_DGRAM;
    f->fd = ::socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (f->fd < 0 || ::connect(f->fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        *why = "the transport of " + a.stream() + " at " + a.address() +
               " cannot be reached: " + std::strerror(errno);
        return nullptr;
    }
    return f;
}

} // namespace

std::string Declaration::manifest() const {
    std::ostringstream out;
    out << "manifest: 1\nid: " << quoted(id) << "\nprotocol: " << plugin_contract() << "\n";
    if (!needs.empty()) {
        out << "needs:\n";
        for (const auto &need : needs) {
            out << "  - " << quoted(need) << "\n";
        }
    }
    if (!streams.empty()) {
        out << "streams:\n";
        for (const auto &s : streams) {
            out << "  - id: " << quoted(s.id) << "\n";
            if (s.tolerates_loss) {
                out << "    tolerates_loss: true\n";
            }
            if (s.tolerates_reorder) {
                out << "    tolerates_reorder: true\n";
            }
        }
    }
    ids(out, "commands", commands);
    ids(out, "queries", queries);
    ids(out, "occurrences", occurrences);
    if (!capabilities.empty()) {
        out << "capabilities:\n";
        for (const auto &c : capabilities) {
            out << "  - name: " << quoted(c.name) << "\n    governs:\n      " << governed(c.governs.kind)
                << ": " << quoted(c.governs.id) << "\n";
        }
    }
    return out.str();
}

struct Unit::State {
    Admission admission;
    std::shared_ptr<grpc::Channel> channel;
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReaderWriter<pluginv1::Envelope, pluginv1::Envelope>> stream;
    int listener = -1;
    std::string bind;
    std::unique_ptr<Envelopes> envelopes;
    std::chrono::nanoseconds interval{0};
    std::thread reader, beater;

    std::mutex mu;
    std::condition_variable changed;
    std::deque<Event> events;
    bool handed_end = false;
    bool ended = false, closing = false;
    Clock::time_point close_by;
    std::map<std::string, std::shared_ptr<Flow>> active;

    // One write at a time, and none once this side of the stream is done.
    std::mutex writing;
    bool writes_done = false;

    // The author's last health report, which every beat repeats; none until one. Held while a report is
    // read and sent, so a beat never sends a report older than one the author has already sent.
    std::mutex reporting;
    std::optional<pluginv1::Health> last;

    void push(Event event) {
        std::lock_guard<std::mutex> lock(mu);
        events.push_back(std::move(event));
        changed.notify_all();
    }

    // Seals e — as an answer to `to`, where there is one — and sends it on the Session.
    void send(pluginv1::Envelope &e, const std::string *to = nullptr) {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (ended) {
                throw Refusal("session.revoked", "the Session has ended");
            }
        }
        if (to != nullptr) {
            envelopes->answer(*to, e);
        } else {
            envelopes->seal(e);
        }
        std::lock_guard<std::mutex> lock(writing);
        if (!writes_done) {
            stream->Write(e);
        }
    }

    void acknowledge(const std::string &to, pluginv1::Ack::Outcome outcome, const std::string &line) {
        pluginv1::Envelope e;
        e.mutable_ack()->set_outcome(outcome);
        e.mutable_ack()->set_line(line);
        try {
            send(e, &to);
        } catch (const Refusal &) {
            // The Session ended while the activation was being answered: nobody is left to tell.
        }
    }

    // Ends the Session once: the end is handed over and nothing follows it.
    void finish(Ended end) {
        std::map<std::string, std::shared_ptr<Flow>> flows;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (ended) {
                return;
            }
            ended = true;
            flows.swap(active);
            events.push_back(std::move(end));
            changed.notify_all();
        }
        for (auto &[name, f] : flows) {
            ::shutdown(f->fd, SHUT_RDWR);
        }
        context.TryCancel();
        if (listener >= 0) {
            ::close(listener);
            listener = -1;
            ::unlink(bind.c_str());
        }
    }

    void handle(const pluginv1::Envelope &e) {
        if (e.has_session() && e.session().has_revoked()) {
            const auto &r = e.session().revoked();
            finish(Ended{false, spoken(pluginv1::SessionMessage::Revoked::Cause_Name(r.cause()), "CAUSE_"),
                         r.line()});
        } else if (e.has_control() && e.control().has_command()) {
            const auto &c = e.control().command();
            push(Command{e.message_id(), c.type(), c.payload()});
        } else if (e.has_control() && e.control().has_activate()) {
            const auto &a = e.control().activate();
            std::string why;
            std::shared_ptr<Flow> f = connect_flow(a, &why);
            if (f == nullptr) {
                acknowledge(e.message_id(), pluginv1::Ack::OUTCOME_FAILED, why);
                return;
            }
            std::shared_ptr<Flow> former;
            {
                std::lock_guard<std::mutex> lock(mu);
                former = std::exchange(active[a.stream()], f);
            }
            if (former != nullptr) {
                ::shutdown(former->fd, SHUT_RDWR);
            }
            acknowledge(e.message_id(), pluginv1::Ack::OUTCOME_DONE, "");
            bool is_framed = a.transport() == pluginv1::Control::Activate::TRANSPORT_FRAMED;
            push(Activated{a.stream(), is_framed ? "framed" : "ordered", a.address()});
        } else if (e.has_control() && e.control().has_stop()) {
            const std::string &stream_id = e.control().stop().stream();
            std::shared_ptr<Flow> f;
            {
                std::lock_guard<std::mutex> lock(mu);
                auto found = active.find(stream_id);
                if (found != active.end()) {
                    f = found->second;
                    active.erase(found);
                }
            }
            if (f != nullptr) {
                ::shutdown(f->fd, SHUT_RDWR);
            }
            acknowledge(e.message_id(), pluginv1::Ack::OUTCOME_DONE, "");
            push(Stopped{stream_id});
        } else if (e.has_query() && e.query().has_question()) {
            const auto &q = e.query().question();
            push(Question{e.message_id(), q.type(), q.payload()});
        } else if (e.has_error()) {
            push(Refused{e.correlation_id(), e.error().code(), e.error().message(),
                         e.error().has_withheld() ? e.error().withheld().item() : std::string()});
        }
    }

    void read() {
        pluginv1::Envelope e;
        while (stream->Read(&e)) {
            handle(e);
            e.Clear();
        }
        {
            // Nothing is written once the stream is being finished.
            std::lock_guard<std::mutex> lock(writing);
            writes_done = true;
        }
        grpc::Status status = stream->Finish();
        bool was_closing;
        {
            std::lock_guard<std::mutex> lock(mu);
            was_closing = closing;
        }
        if (was_closing) {
            finish(Ended{true, {}, {}});
        } else {
            std::string said = status.ok() ? "the Core ended it" : status.error_message();
            finish(Ended{false, "liveness lost", "the Session's stream ended: " + said});
        }
    }

    // Repeats the author's last health report at the interval the Core assigned, until the Session ends,
    // and ends a close the Core does not answer. Before the author's first report it sends nothing: a
    // grade is the author's statement, and a unit that never reports loses its liveness as a unit that
    // sends nothing does.
    void beat() {
        std::optional<Clock::time_point> next_beat;
        if (interval.count() > 0) {
            next_beat = Clock::now() + interval;
        }
        std::unique_lock<std::mutex> lock(mu);
        while (!ended) {
            std::optional<Clock::time_point> wake = next_beat;
            if (closing && (!wake || close_by < *wake)) {
                wake = close_by;
            }
            if (!wake) {
                changed.wait(lock);
                continue;
            }
            if (Clock::now() < *wake) {
                changed.wait_until(lock, *wake);
                continue;
            }
            if (closing && Clock::now() >= close_by) {
                // The Core ends the stream on a CLOSE; if it does not, the departure still is one.
                lock.unlock();
                finish(Ended{true, {}, {}});
                lock.lock();
                continue;
            }
            if (next_beat && Clock::now() >= *next_beat) {
                *next_beat += interval;
                lock.unlock();
                {
                    std::lock_guard<std::mutex> report(reporting);
                    if (last) {
                        pluginv1::Envelope e;
                        *e.mutable_health() = *last;
                        try {
                            send(e);
                        } catch (const Refusal &) {
                        }
                    }
                }
                lock.lock();
            }
        }
    }
};

Unit::Unit(std::unique_ptr<State> state) : state_(std::move(state)) {}

std::unique_ptr<Unit> Unit::start(const Declaration &declaration) {
    return start(declaration, [](const char *name) { return std::getenv(name); });
}

std::unique_ptr<Unit> Unit::start(const Declaration &declaration, const Getenv &getenv) {
    Env env = environment(getenv);
    // Bind before registering: registered and unreachable is the one order that is wrong.
    int listener = bind_unit(env.bind);
    auto release = [&] {
        ::close(listener);
        ::unlink(env.bind.c_str());
    };

    pluginv1::RegisterRequest request;
    request.set_plugin(env.plugin);
    request.set_unit(env.unit);
    request.set_token(env.token);
    request.set_protocol(static_cast<std::uint32_t>(plugin_contract()));
    request.set_language("cpp");
    request.set_sdk_line(sdk_line);
    // The surface the registration claims: the declaration again, from the same value.
    auto *claimed = request.mutable_declared();
    for (const auto &c : declaration.capabilities) {
        claimed->add_capabilities(c.name);
    }
    for (const auto &s : declaration.streams) {
        claimed->add_streams(s.id);
    }
    claimed->mutable_commands()->Add(declaration.commands.begin(), declaration.commands.end());
    claimed->mutable_queries()->Add(declaration.queries.begin(), declaration.queries.end());
    claimed->mutable_occurrences()->Add(declaration.occurrences.begin(), declaration.occurrences.end());

    auto channel = dial(env.socket);
    pluginv1::RegisterResponse response;
    grpc::ClientContext registering;
    grpc::Status status = pluginv1::Register::NewStub(channel)->Register(&registering, request, &response);
    if (!status.ok()) {
        release();
        throw std::runtime_error("the registration at " + env.socket + " failed: " + status.error_message());
    }
    if (response.outcome() == pluginv1::RegisterResponse::OUTCOME_REFUSED) {
        release();
        throw Refusal(response.code(), response.message(),
                      spoken(pluginv1::Stage_Name(response.stage()), "STAGE_"));
    }

    auto state = std::make_unique<State>();
    State &s = *state;
    s.admission.restricted =
        response.outcome() == pluginv1::RegisterResponse::OUTCOME_ACCEPTED_WITH_RESTRICTIONS;
    s.admission.granted = scope_of(response.granted());
    s.admission.withheld = scope_of(response.withheld());
    if (response.has_heartbeat()) {
        const auto &i = response.heartbeat().interval();
        s.interval = std::chrono::seconds(i.seconds()) + std::chrono::nanoseconds(i.nanos());
    }
    s.envelopes = std::make_unique<Envelopes>(response.session_id());
    s.channel = channel;
    s.listener = listener;
    s.bind = env.bind;
    s.stream = pluginv1::Session::NewStub(channel)->Open(&s.context);

    pluginv1::Envelope open;
    open.mutable_session()->mutable_open();
    s.send(open);
    s.reader = std::thread([&s] { s.read(); });
    s.beater = std::thread([&s] { s.beat(); });
    return std::unique_ptr<Unit>(new Unit(std::move(state)));
}

Unit::~Unit() {
    State &s = *state_;
    s.finish(Ended{true, {}, {}});
    if (s.reader.joinable()) {
        s.reader.join();
    }
    if (s.beater.joinable()) {
        s.beater.join();
    }
}

const Admission &Unit::admission() const { return state_->admission; }

std::optional<Event> Unit::next() {
    State &s = *state_;
    std::unique_lock<std::mutex> lock(s.mu);
    s.changed.wait(lock, [&s] { return !s.events.empty() || s.handed_end; });
    if (s.events.empty()) {
        return std::nullopt;
    }
    Event event = std::move(s.events.front());
    s.events.pop_front();
    s.handed_end = s.handed_end || std::holds_alternative<Ended>(event);
    return event;
}

std::optional<Event> Unit::next(std::chrono::milliseconds wait) {
    State &s = *state_;
    std::unique_lock<std::mutex> lock(s.mu);
    if (!s.changed.wait_for(lock, wait, [&s] { return !s.events.empty() || s.handed_end; }) ||
        s.events.empty()) {
        return std::nullopt;
    }
    Event event = std::move(s.events.front());
    s.events.pop_front();
    s.handed_end = s.handed_end || std::holds_alternative<Ended>(event);
    return event;
}

bool Unit::done() const {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->ended;
}

void Unit::close() {
    State &s = *state_;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        if (s.ended) {
            return;
        }
        s.closing = true;
        s.close_by = Clock::now() + close_grace;
        s.changed.notify_all();
    }
    pluginv1::Envelope e;
    e.mutable_session()->mutable_close();
    s.send(e);
    std::lock_guard<std::mutex> lock(s.writing);
    if (!s.writes_done) {
        s.stream->WritesDone();
        s.writes_done = true;
    }
}

void Unit::ack(const Command &command, Outcome outcome, std::string_view line) {
    pluginv1::Envelope e;
    e.mutable_ack()->set_outcome(static_cast<pluginv1::Ack::Outcome>(outcome));
    e.mutable_ack()->set_line(std::string(line));
    state_->send(e, &command.id);
}

void Unit::answer(const Question &question, std::string_view payload) {
    pluginv1::Envelope e;
    e.mutable_query()->mutable_answer()->set_payload(std::string(payload));
    state_->send(e, &question.id);
}

void Unit::fail(std::string_view about, std::string_view code, std::string_view message) {
    pluginv1::Envelope e;
    e.mutable_error()->set_code(std::string(code));
    e.mutable_error()->set_message(std::string(message));
    const std::string to(about);
    state_->send(e, &to);
}

void Unit::report(std::string_view occurrence, Severity severity, std::string_view line,
                  std::string_view detail) {
    if (!severity.stated()) {
        throw std::runtime_error("an occurrence is reported with the author's severity, and none was stated");
    }
    if (severity.value() > 99) {
        throw std::runtime_error("a severity runs from 0 to 99, and " + std::to_string(severity.value()) +
                                 " is not one");
    }
    pluginv1::Envelope e;
    auto *event = e.mutable_event();
    event->set_occurrence(std::string(occurrence));
    event->set_severity(severity.value());
    event->set_line(std::string(line));
    event->set_detail(std::string(detail));
    state_->send(e);
}

void Unit::health(unsigned grade, std::string_view line) {
    if (grade > 99) {
        throw std::runtime_error("a grade runs from 0 to 99, and " + std::to_string(grade) + " is not one");
    }
    State &s = *state_;
    std::lock_guard<std::mutex> lock(s.reporting);
    pluginv1::Health health;
    health.set_grade(grade);
    health.set_line(std::string(line));
    s.last = health;
    pluginv1::Envelope e;
    *e.mutable_health() = health;
    s.send(e);
}

void Unit::emit(std::string_view stream, std::string_view payload) {
    State &s = *state_;
    std::shared_ptr<Flow> f;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        auto found = s.active.find(std::string(stream));
        if (found != s.active.end()) {
            f = found->second;
        }
    }
    if (f == nullptr) {
        throw Refusal("stream.inactive", "the stream " + std::string(stream) + " has not been activated");
    }
    std::lock_guard<std::mutex> lock(f->mu);
    std::uint64_t sequence = ++f->next;
    std::string message;
    if (f->framed) {
        message.resize(16);
        auto clock = static_cast<std::uint64_t>(now_ns());
        for (int i = 0; i < 8; i++) {
            message[i] = static_cast<char>(sequence >> (8 * i));
            message[8 + i] = static_cast<char>(clock >> (8 * i));
        }
        message.append(payload);
    } else {
        pluginv1::Envelope e;
        e.mutable_data()->set_sequence(sequence);
        e.mutable_data()->set_payload(std::string(payload));
        s.envelopes->seal(e);
        message = e.SerializeAsString();
    }
    if (::send(f->fd, message.data(), message.size(), MSG_NOSIGNAL) < 0) {
        throw Refusal("stream.inactive",
                      "the transport of " + std::string(stream) + " took nothing: " + std::strerror(errno));
    }
}

} // namespace yoke::plugin
