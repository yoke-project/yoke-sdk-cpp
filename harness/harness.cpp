#include "harness.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace harness {

using json = nlohmann::json;
namespace plugin = yoke::plugin;

const plugin::Declaration &declaration() {
    static const plugin::Declaration d{
        "com.yoke.conformance.cpp",
        {},
        {{"conformance.data"}, {"conformance.frames", true}},
        {"calibrate"},
        {"status"},
        {"conformance.drift"},
        {
            {"stream.data.publish", {plugin::Governs::stream, "conformance.data"}},
            {"stream.frames.publish", {plugin::Governs::stream, "conformance.frames"}},
            {"command.calibrate.accept", {plugin::Governs::command, "calibrate"}},
            {"query.status.answer", {plugin::Governs::query, "status"}},
            {"event.drift.report", {plugin::Governs::occurrence, "conformance.drift"}},
        },
    };
    return d;
}

namespace {

int asked_pipe = -1;
volatile std::sig_atomic_t asked = 0;

void on_term(int) {
    asked = 1;
    if (asked_pipe >= 0) {
        [[maybe_unused]] ssize_t n = write(asked_pipe, "", 1);
    }
}

// The harness's state: the control socket, the unit once started, and what it was sent to answer.
struct Harness {
    int conn = -1;
    int wake[2] = {-1, -1}; // written when the Session ends, or the process is asked to stop
    std::mutex mu;          // the socket's writes, and the held events
    std::unique_ptr<plugin::Unit> unit;
    std::map<std::string, plugin::Command> commands;
    std::map<std::string, plugin::Question> questions;
    std::atomic<bool> ended{false};

    void send(const json &line) {
        std::string s = line.dump() + "\n";
        std::lock_guard<std::mutex> hold(mu);
        for (std::size_t off = 0; off < s.size();) {
            ssize_t n = write(conn, s.data() + off, s.size() - off);
            if (n <= 0) {
                break;
            }
            off += static_cast<std::size_t>(n);
        }
    }

    // Reports everything the library surfaces, in the order it surfaced it.
    void observe() {
        while (auto event = unit->next()) {
            json fields;
            std::string kind;
            bool end = false;
            if (auto *c = std::get_if<plugin::Command>(&*event)) {
                kind = "command", fields = {{"id", c->id}, {"type", c->type}};
                std::lock_guard<std::mutex> hold(mu);
                commands[c->id] = *c;
            } else if (auto *q = std::get_if<plugin::Question>(&*event)) {
                kind = "question", fields = {{"id", q->id}, {"type", q->type}, {"payload", q->payload}};
                std::lock_guard<std::mutex> hold(mu);
                questions[q->id] = *q;
            } else if (auto *a = std::get_if<plugin::Activated>(&*event)) {
                kind = "activated", fields = {{"stream", a->stream}, {"transport", a->transport}};
            } else if (auto *s = std::get_if<plugin::Stopped>(&*event)) {
                kind = "stopped", fields = {{"stream", s->stream}};
            } else if (auto *r = std::get_if<plugin::Refused>(&*event)) {
                kind = "refused", fields = {{"correlation", r->correlation}, {"code", r->code}};
            } else if (auto *e = std::get_if<plugin::Ended>(&*event)) {
                kind = "session-ended", fields = {{"closed", e->closed}, {"cause", e->cause}}, end = true;
            }
            send({{"type", "observation"}, {"kind", kind}, {"fields", fields}});
            if (end) {
                break;
            }
        }
        // The Session ended: the incarnation is over, and so is the process.
        ended = true;
        [[maybe_unused]] ssize_t n = write(wake[1], "", 1);
    }

    static std::string arg(const json &args, const char *name) {
        auto it = args.find(name);
        return it != args.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    // Turns a directive into a library call, and fills result with what the library returned. A refusal
    // is reported as its code, and never as the library's message.
    void act(const std::string &verb, const json &args, const yoke::Getenv &getenv, json &result) {
        try {
            if (verb == "describe") {
                result["value"] = {{"manifest", declaration().manifest()}};
                return;
            }
            if (verb == "start") {
                auto u = plugin::Unit::start(declaration(), getenv);
                const plugin::Admission a = u->admission();
                if (!unit) {
                    unit = std::move(u);
                    std::thread([this] { observe(); }).detach();
                }
                auto scope = [](const plugin::Scope &s) {
                    return json{{"capabilities", s.capabilities},
                                {"streams", s.streams},
                                {"commands", s.commands},
                                {"queries", s.queries}};
                };
                result["value"] = {{"outcome", a.restricted ? "accepted with restrictions" : "accepted"},
                                   {"granted", scope(a.granted)},
                                   {"withheld", scope(a.withheld)}};
                return;
            }
            static const char *const acts[] = {"close", "emit", "report-health", "report", "ack", "answer"};
            bool known = false;
            for (const char *a : acts) {
                known = known || verb == a;
            }
            if (!known) {
                result["unrecognised"] = true;
                return;
            }
            if (!unit) {
                result["value"] = {{"failed", true}, {"started", false}};
                return;
            }
            if (verb == "close") {
                unit->close();
            } else if (verb == "emit") {
                unit->emit(arg(args, "stream"), arg(args, "payload"));
            } else if (verb == "report-health") {
                unsigned grade =
                    args.contains("grade") && args["grade"].is_number() ? args["grade"].get<unsigned>() : 0;
                unit->health(grade, arg(args, "line"));
            } else if (verb == "report") {
                plugin::Severity severity;
                if (args.contains("severity") && args["severity"].is_number()) {
                    severity = plugin::Severity::of(args["severity"].get<unsigned>());
                }
                unit->report(arg(args, "occurrence"), severity, arg(args, "line"));
            } else if (verb == "ack") {
                plugin::Command c;
                {
                    std::lock_guard<std::mutex> hold(mu);
                    auto it = commands.find(arg(args, "command"));
                    if (it != commands.end()) {
                        c = it->second;
                    }
                }
                unit->ack(c, plugin::Outcome::done, arg(args, "line"));
            } else {
                plugin::Question q;
                {
                    std::lock_guard<std::mutex> hold(mu);
                    auto it = questions.find(arg(args, "question"));
                    if (it != questions.end()) {
                        q = it->second;
                    }
                }
                unit->answer(q, arg(args, "payload"));
            }
            result["value"] = json::object();
        } catch (const yoke::Refusal &r) {
            result["refusal"] = r.code();
            result["value"] = r.stage().empty() ? json::object() : json{{"stage", r.stage()}};
        } catch (const std::exception &) {
            result["value"] = {{"failed", true}};
        }
    }
};

// Waits on fd for at most ms; whether it was woken.
bool woken(int fd, int ms) {
    pollfd p{fd, POLLIN, 0};
    int n;
    do {
        n = poll(&p, 1, ms);
    } while (n < 0 && errno == EINTR);
    return n > 0;
}

} // namespace

int serve(const yoke::Getenv &getenv) {
    const char *path = getenv("CONFORMANCE_SOCKET");
    sockaddr_un addr{};
    if (!path || std::strlen(path) >= sizeof addr.sun_path) {
        return 1;
    }
    Harness h;
    addr.sun_family = AF_UNIX;
    std::strcpy(addr.sun_path, path);
    h.conn = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (h.conn < 0 || connect(h.conn, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 ||
        pipe(h.wake) != 0) {
        return 1;
    }
    asked_pipe = h.wake[1];
    struct sigaction sa {};
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, nullptr);

    json hello = {{"type", "hello"},
                  {"contract", "plugin"},
                  {"language", "cpp"},
                  {"sdk", yoke::sdk_line},
                  {"version", yoke::plugin_contract()}};
    if (const char *unit = getenv("YOKE_UNIT")) {
        hello["unit"] = unit;
    }
    h.send(hello);

    std::string pending;
    char buf[65536];
    for (;;) {
        pollfd p[2] = {{h.conn, POLLIN, 0}, {h.wake[0], POLLIN, 0}};
        if (poll(p, 2, -1) < 0 && errno != EINTR) {
            return 1;
        }
        if (asked) {
            // Asked to stop, as the Core asks a process whose Session has ended: the end is reported
            // first, and the process leaves within the Core's window.
            auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!h.ended && std::chrono::steady_clock::now() < until) {
                woken(h.wake[0], 100);
            }
            return 0;
        }
        if (h.ended) {
            return 0;
        }
        if (!(p[0].revents & (POLLIN | POLLHUP))) {
            continue;
        }
        ssize_t n = read(h.conn, buf, sizeof buf);
        if (n <= 0) {
            if (h.unit) {
                try {
                    h.unit->close();
                } catch (...) {
                }
            }
            return 0;
        }
        pending.append(buf, static_cast<std::size_t>(n));
        for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            json d = json::parse(pending.substr(0, nl), nullptr, false);
            pending.erase(0, nl + 1);
            if (!d.is_object() || !d.contains("type")) {
                continue;
            }
            if (d["type"] == "finish") {
                if (h.unit) {
                    try {
                        h.unit->close();
                    } catch (...) {
                    }
                }
                return 0;
            }
            if (d["type"] != "directive" || !d.contains("verb") || !d["verb"].is_string()) {
                continue;
            }
            json result = {{"type", "result"}};
            if (d.contains("id")) {
                result["id"] = d["id"];
            }
            h.act(d["verb"].get<std::string>(), d.value("args", json::object()), getenv, result);
            h.send(result);
        }
    }
}

} // namespace harness
