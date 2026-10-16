#include "interface_harness.hpp"

#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace interface_harness {

using json = nlohmann::json;
namespace yi = yoke::iface;
namespace v1 = yi::v1;

namespace {

// A message of the contract as the suite reads it: its fields by their names in the contract.
json plain(const google::protobuf::Message &m) {
    std::string out;
    google::protobuf::util::JsonPrintOptions o;
    o.preserve_proto_field_names = true;
    if (!google::protobuf::util::MessageToJsonString(m, &out, o).ok()) {
        return {{"unreadable", true}};
    }
    json parsed = json::parse(out, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

json records(const std::vector<v1::Record> &rs) {
    json out = json::array();
    for (const auto &r : rs) {
        out.push_back(plain(r));
    }
    return out;
}

json records(const google::protobuf::RepeatedPtrField<v1::Record> &rs) {
    return records(std::vector<v1::Record>(rs.begin(), rs.end()));
}

std::string text(const json &args, const char *key) {
    auto it = args.find(key);
    return it != args.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// A number the suite sent as a number, or as the string a 64-bit value is written as.
std::uint64_t number(const json &args, const char *key) {
    auto it = args.find(key);
    if (it == args.end()) {
        return 0;
    }
    if (it->is_number_unsigned() || it->is_number_integer()) {
        return it->get<std::uint64_t>();
    }
    if (it->is_number_float()) {
        return static_cast<std::uint64_t>(it->get<double>());
    }
    if (it->is_string()) {
        try {
            return std::stoull(it->get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

// An event as the suite reads it.
json event(const v1::Event &e) {
    return {{"seq", e.seq()},
            {"type", e.type()},
            {"subject_kind", e.subject().kind()},
            {"subject", e.subject().identity()},
            {"severity", e.severity()}};
}

// A refusal as its code and what it names; anything else the library failed with is a failure the
// harness does not interpret.
json refused(const std::exception_ptr &failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const yoke::Refusal &r) {
        json value = json::object();
        if (!r.subject().kind.empty()) {
            value["subject"] = {{"kind", r.subject().kind},
                                {"identity", r.subject().identity},
                                {"incarnation", r.subject().incarnation}};
        }
        if (!r.item().empty()) {
            value["item"] = r.item();
        }
        if (auto *s = dynamic_cast<const yi::Suspended *>(&r)) {
            value["suspension"] = {{"grade", s->grade()}, {"by", s->by()}};
        }
        return {{"refusal", r.code()}, {"value", value}};
    } catch (...) {
        return {{"value", {{"failed", true}}}};
    }
}

// What the harness holds between directives: its attachment, once one is made, and the threads
// reporting what subscriptions deliver.
struct Harness {
    yoke::Getenv getenv;
    int conn = -1;
    std::mutex writing;
    std::unique_ptr<yi::Attachment> a;
    std::vector<std::unique_ptr<yi::Attachment>> extra;
    std::vector<std::unique_ptr<yi::Subscription>> subscriptions;
    std::vector<std::unique_ptr<yi::Delivery>> deliveries;
    std::vector<std::thread> reporting;

    ~Harness() {
        // Every attachment and subscription lives until the harness finishes; closing them ends what
        // reports them.
        for (auto &s : subscriptions) {
            s->close();
        }
        for (auto &x : extra) {
            x->close();
        }
        if (a) {
            a->close();
        }
        for (auto &t : reporting) {
            t.join();
        }
    }

    void send(const json &line) {
        std::string s = line.dump() + "\n";
        std::lock_guard<std::mutex> hold(writing);
        for (std::size_t off = 0; off < s.size();) {
            ssize_t n = write(conn, s.data() + off, s.size() - off);
            if (n <= 0) {
                break;
            }
            off += static_cast<std::size_t>(n);
        }
    }

    void observe(const char *kind, const json &fields) {
        send({{"type", "observation"}, {"kind", kind}, {"fields", fields}});
    }

    // Reports what a subscription delivers, in order, until it ends.
    void subscribed(yi::Subscription *sub) {
        reporting.emplace_back([this, sub] {
            for (;;) {
                std::optional<v1::Subscribed> d;
                try {
                    d = sub->next();
                } catch (...) {
                }
                if (!d) {
                    observe("ended", json::object());
                    return;
                }
                if (d->has_snapshot()) {
                    observe("snapshot",
                            {{"at", d->snapshot().at()}, {"records", records(d->snapshot().records())}});
                } else if (d->has_overflow()) {
                    observe("overflow",
                            {{"at", d->overflow().at()}, {"records", records(d->overflow().records())}});
                } else if (d->has_event()) {
                    observe("event", event(d->event()));
                }
            }
        });
    }

    // Attaches to the channel named, at the address the library computes under the instance.
    json attach(const std::string &channel) {
        const char *instance = getenv("CONFORMANCE_INSTANCE");
        std::string address;
        try {
            address = yi::service(instance != nullptr ? instance : "", channel);
        } catch (...) {
            return {{"value", {{"failed", true}}}};
        }
        std::unique_ptr<yi::Attachment> attached;
        try {
            attached = yi::Attachment::attach(address);
        } catch (...) {
            return refused(std::current_exception());
        }
        std::string standing = attached->standing() ? attached->standing()->id() : "";
        json value = {{"version", attached->version()},
                      {"subscription", standing},
                      {"at", attached->picture().at()},
                      {"records", records(attached->picture().records())}};
        // A second attachment the channel admitted is kept beside the first, which stays the one directed.
        if (a) {
            extra.push_back(std::move(attached));
        } else {
            a = std::move(attached);
            if (a->standing()) {
                subscribed(a->standing());
            }
        }
        return {{"value", value}};
    }

    // Performs one directive, and reports what the library answered.
    json act(const std::string &verb, const json &args) {
        if (verb == "attach") {
            return attach(text(args, "channel"));
        }
        static const char *const operations[] = {"authenticate",     "read",   "subscribe",    "confirm",
                                                 "command",          "query",  "stream-start", "stream-stop",
                                                 "stream-subscribe", "reclaim"};
        bool known = false;
        for (const char *op : operations) {
            known = known || verb == op;
        }
        if (!known) {
            return {{"unrecognised", true}};
        }
        if (!a) {
            return {{"value", {{"failed", true}, {"attached", false}}}};
        }
        try {
            if (verb == "authenticate") {
                std::string token = text(args, "token");
                v1::Authenticated got = token.empty()
                                            ? a->authenticate(text(args, "account"), text(args, "secret"))
                                            : a->authenticate_token(token);
                return {{"value", plain(got)}};
            }
            if (verb == "read") {
                return {
                    {"value", {{"records", records(a->read(text(args, "kind"), text(args, "identity")))}}}};
            }
            if (verb == "subscribe") {
                v1::Filter f;
                f.set_subject_kind(text(args, "subject_kind"));
                f.set_subject_identity(text(args, "subject_identity"));
                f.set_type(text(args, "type"));
                subscriptions.push_back(a->subscribe(f));
                subscribed(subscriptions.back().get());
                return {{"value", json::object()}};
            }
            if (verb == "confirm") {
                std::string subscription = text(args, "subscription");
                if (subscription.empty() && a->standing()) {
                    subscription = a->standing()->id();
                }
                a->confirm(subscription, number(args, "sequence"));
                return {{"value", json::object()}};
            }
            if (verb == "command") {
                return {{"value",
                         plain(a->command(text(args, "unit"), text(args, "type"), text(args, "payload")))}};
            }
            if (verb == "query") {
                return {
                    {"value",
                     {{"answer", a->ask(text(args, "unit"), text(args, "type"), text(args, "payload"))}}}};
            }
            if (verb == "stream-start") {
                return {{"value", plain(a->start_stream(text(args, "unit"), text(args, "stream")))}};
            }
            if (verb == "stream-stop") {
                return {{"value", plain(a->stop_stream(text(args, "unit"), text(args, "stream")))}};
            }
            if (verb == "stream-subscribe") {
                deliveries.push_back(a->subscribe_stream(text(args, "unit"), text(args, "stream")));
                yi::Delivery *d = deliveries.back().get();
                reporting.emplace_back([this, d] {
                    for (;;) {
                        std::optional<yi::Frame> f;
                        try {
                            f = d->next();
                        } catch (...) {
                        }
                        if (!f) {
                            observe("delivery-ended", {{"delivery", d->id()}});
                            return;
                        }
                        observe("frame", {{"delivery", d->id()},
                                          {"sequence", f->sequence},
                                          {"sent_at", f->sent_at},
                                          {"payload", f->payload}});
                    }
                });
                return {{"value", {{"delivery", d->id()}, {"flowing", d->flowing()}}}};
            }
            return {{"value", plain(a->reclaim())}};
        } catch (...) {
            return refused(std::current_exception());
        }
    }
};

} // namespace

int serve(const yoke::Getenv &getenv) {
    const char *path = getenv("CONFORMANCE_SOCKET");
    sockaddr_un addr{};
    if (!path || std::strlen(path) >= sizeof addr.sun_path) {
        return 1;
    }
    addr.sun_family = AF_UNIX;
    std::strcpy(addr.sun_path, path);
    int conn = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (conn < 0 || connect(conn, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        return 1;
    }
    int status = 0;
    {
        Harness h;
        h.getenv = getenv;
        h.conn = conn;
        h.send({{"type", "hello"},
                {"contract", "interface"},
                {"language", "cpp"},
                {"sdk", yoke::sdk_line},
                {"version", yi::contract()}});
        std::string pending;
        char buf[65536];
        bool finished = false;
        while (!finished) {
            ssize_t n = read(conn, buf, sizeof buf);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            pending.append(buf, static_cast<std::size_t>(n));
            for (std::size_t nl; !finished && (nl = pending.find('\n')) != std::string::npos;) {
                json d = json::parse(pending.substr(0, nl), nullptr, false);
                pending.erase(0, nl + 1);
                if (!d.is_object() || !d.contains("type")) {
                    continue;
                }
                if (d["type"] == "finish") {
                    finished = true;
                    break;
                }
                if (d["type"] != "directive" || !d.contains("verb") || !d["verb"].is_string()) {
                    continue;
                }
                json result = h.act(d["verb"].get<std::string>(), d.value("args", json::object()));
                result["type"] = "result";
                if (d.contains("id")) {
                    result["id"] = d["id"];
                }
                h.send(result);
            }
        }
    }
    close(conn);
    return status;
}

} // namespace interface_harness
