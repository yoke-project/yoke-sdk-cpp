#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.hpp"
#include "core.hpp"
#include "yoke/interface.hpp"

using namespace std::chrono_literals;
namespace yi = yoke::iface;

namespace {

// A Core served on a local socket in the case's directory, and its path.
struct Served {
    std::string path;
    std::unique_ptr<grpc::Server> server;

    Served(Core &core, const std::string &name = "panel.sock") : path(check::dir() + "/" + name) {
        server = serve(core, path);
        REQUIRE(server != nullptr, "the Core could not be served at ", path);
    }
    ~Served() { server->Shutdown(std::chrono::system_clock::now() + 1s); }
};

yoke::Getenv env(std::map<std::string, std::string> values) {
    auto held = std::make_shared<std::map<std::string, std::string>>(std::move(values));
    return [held](const char *name) -> const char * {
        auto it = held->find(name);
        return it == held->end() ? nullptr : it->second.c_str();
    };
}

template <typename F> std::string failure_of(F f) {
    try {
        f();
    } catch (const std::exception &e) {
        return e.what();
    }
    return "";
}

template <typename F> std::string refusal_of(F f) {
    try {
        f();
    } catch (const yoke::Refusal &r) {
        return r.code();
    } catch (const std::exception &e) {
        return std::string("not a refusal: ") + e.what();
    }
    return "";
}

bool contains(const std::string &s, const std::string &sub) { return s.find(sub) != std::string::npos; }

iv1::Response answer_of(const iv1::Request &r) {
    iv1::Response resp;
    switch (r.operation_case()) {
    case iv1::Request::kAuthenticate:
        resp.mutable_authenticate()->set_account("ada");
        break;
    case iv1::Request::kRead:
        resp.mutable_read();
        break;
    case iv1::Request::kSubscribe:
        resp.mutable_subscribe()->mutable_snapshot();
        break;
    case iv1::Request::kConfirm:
        resp.mutable_confirm();
        break;
    case iv1::Request::kCommand:
        resp.mutable_command()->set_outcome(iv1::Acknowledged::OUTCOME_DONE);
        break;
    case iv1::Request::kQuery:
        resp.mutable_query()->set_payload(r.query().unit());
        break;
    case iv1::Request::kStreamStart:
        resp.mutable_stream_start()->set_outcome(iv1::Acknowledged::OUTCOME_DONE);
        break;
    case iv1::Request::kStreamStop:
        resp.mutable_stream_stop()->set_outcome(iv1::Acknowledged::OUTCOME_DONE);
        break;
    case iv1::Request::kStreamSubscribe:
        resp.mutable_stream_subscribe()->set_delivery("d-1");
        resp.mutable_stream_subscribe()->set_connection(true);
        break;
    case iv1::Request::kStreamUnsubscribe:
        resp.mutable_stream_unsubscribe();
        break;
    case iv1::Request::kReclaim:
        resp.mutable_reclaim()->set_changed(true);
        break;
    default:
        break;
    }
    return resp;
}

// std: yoke-sdk-cpp:the-interface-library.01
void test_a_channels_address_is_computed_or_handed() {
    REQUIRE(yi::service("/run/yoke", "panel") == "/run/yoke/interfaces/panel.sock",
            "the service form's address is ", yi::service("/run/yoke", "panel"));
    auto getenv =
        env({{"XDG_RUNTIME_DIR", "/run/user/1000"}, {"YOKE_SOCKET", "/run/yoke/interfaces/kiosk.sock"}});
    REQUIRE(yi::application("bench-a", "panel", getenv) ==
                "/run/user/1000/yoke/bench-a/interfaces/panel.sock",
            "an application instance's address is ", yi::application("bench-a", "panel", getenv));
    REQUIRE(yi::managed(getenv) == "/run/yoke/interfaces/kiosk.sock", "a managed interface's address is ",
            yi::managed(getenv));
    std::string why = failure_of([] { yi::managed(env({})); });
    REQUIRE(contains(why, "YOKE_SOCKET"), "a managed interface handed no socket was answered '", why, "'");
    REQUIRE(!failure_of([] { yi::service("/run/yoke", "a/b"); }).empty(),
            "a channel name holding a / was given an address");
}

// std: yoke-sdk-cpp:the-interface-library.02
void test_attaching_surfaces_the_opening() {
    Core good;
    good.opening = opening_at(1);
    Served s(good);
    auto a = yi::Attachment::attach(s.path, {}, 3s);
    REQUIRE(a->version() == 1, "the opening surfaced version ", a->version());
    REQUIRE(a->standing() != nullptr && a->standing()->id() == "standing",
            "the standing subscription is not surfaced");
    REQUIRE(a->picture().records_size() == 1 &&
                a->picture().records(0).unit().declared().identity() == "acquire",
            "the picture is ", a->picture().ShortDebugString());
    a->close();

    Core newer;
    newer.opening = opening_at(2);
    Served s2(newer, "newer.sock");
    std::string why;
    try {
        yi::Attachment::attach(s2.path, {}, 3s);
    } catch (const yoke::Refusal &r) {
        why = r.code() + " " + r.what();
    } catch (const std::exception &e) {
        why = std::string("not a refusal: ") + e.what();
    }
    REQUIRE(contains(why, "compat.unsupported") && contains(why, "1") && contains(why, "2"),
            "an opening at version 2 was answered '", why, "'");

    Core busy;
    iv1::Refusal r;
    r.set_code("channel.in_use");
    r.set_message("the channel takes one client");
    busy.refuse = r;
    Served s3(busy, "busy.sock");
    std::string code = refusal_of([&] { yi::Attachment::attach(s3.path, {}, 3s); });
    REQUIRE(code == "channel.in_use", "a refused attachment was answered '", code, "'");
}

// std: yoke-sdk-cpp:the-interface-library.03
void test_one_member_per_operation_on_one_connection() {
    Core c;
    c.opening = opening_at(1);
    // The question about slow is answered only once the command to fast has been.
    auto fast = std::make_shared<std::promise<void>>();
    std::shared_future<void> fast_done = fast->get_future().share();
    std::atomic<bool> gate{false};
    c.answer = [&](const std::string &call, const iv1::Request &r, const Send &send) {
        if (gate && r.has_query() && r.query().unit() == "slow") {
            fast_done.wait();
        }
        send(answered(call, answer_of(r)));
        if (gate && r.has_command() && r.command().unit() == "fast") {
            fast->set_value();
        }
    };
    Served s(c);
    auto a = yi::Attachment::attach(s.path, {}, 3s);
    a->authenticate("ada", "secret");
    a->read("unit");
    a->subscribe(iv1::Filter())->close();
    a->confirm("standing", 3);
    a->command("acquire", "calibrate");
    a->ask("acquire", "status");
    a->start_stream("acquire", "station.spectra");
    a->stop_stream("acquire", "station.spectra");
    a->subscribe_stream("acquire", "station.spectra")->release();
    a->reclaim();

    std::map<std::string, int> sent;
    for (const auto &r : c.requests()) {
        REQUIRE(r.version() == 1, "a request states the version ", r.version());
        sent[yi::name(r)]++;
    }
    for (const char *op : {"authenticate", "read", "subscribe", "confirm", "command", "query", "stream.start",
                           "stream.stop", "stream.subscribe", "stream.unsubscribe", "reclaim"}) {
        REQUIRE(sent[op] == 1, op, " was sent ", sent[op], " times");
    }
    REQUIRE(c.attachments() == 1, "the Core saw ", c.attachments(), " attachments");

    gate = true;
    auto asked = std::async(std::launch::async, [&] {
        try {
            return a->ask("slow", "status");
        } catch (const std::exception &e) {
            return std::string(e.what());
        }
    });
    check::sleep(50);
    std::string why = failure_of([&] { a->command("fast", "calibrate", {}, 2s); });
    REQUIRE(why.empty(), "the command waited behind the question, and was answered '", why, "'");
    std::string got = asked.get();
    REQUIRE(got == "slow", "the question was answered '", got, "'");
}

// std: yoke-sdk-cpp:the-interface-library.04
void test_a_refusal_is_the_bases_error_with_its_suspension() {
    Core c;
    c.opening = opening_at(1);
    c.answer = [](const std::string &call, const iv1::Request &r, const Send &send) {
        iv1::CoreFrame f;
        f.set_call(call);
        if (r.has_command()) {
            f.mutable_refusal()->set_code("channel.suspended");
            f.mutable_refusal()->set_message("suspended");
            f.mutable_refusal()->mutable_suspension()->set_grade("read-only");
            f.mutable_refusal()->mutable_suspension()->set_by("service");
        } else {
            f.mutable_refusal()->set_code("subject.unknown");
            f.mutable_refusal()->set_message("no unit nobody");
            f.mutable_refusal()->mutable_subject()->set_kind("unit");
            f.mutable_refusal()->mutable_subject()->set_identity("nobody");
        }
        send(f);
    };
    Served s(c);
    auto a = yi::Attachment::attach(s.path, {}, 3s);
    bool read_refused = false;
    try {
        a->read("unit", "nobody");
    } catch (const yoke::Refusal &r) {
        read_refused =
            r.code() == "subject.unknown" && r.subject().kind == "unit" && r.subject().identity == "nobody";
    }
    REQUIRE(read_refused, "a read of nobody was not refused subject.unknown naming the unit nobody");
    bool suspended = false;
    try {
        a->command("acquire", "calibrate");
    } catch (const yi::Suspended &r) {
        suspended = r.code() == "channel.suspended" && r.grade() == "read-only" && r.by() == "service";
        const yoke::Refusal &base = r;
        suspended = suspended && base.code() == "channel.suspended";
    }
    REQUIRE(suspended,
            "a command on a suspended channel did not surface its grade and the channel it favours");
}

// std: yoke-sdk-cpp:the-interface-library.05
void test_a_subscription_is_delivered_in_order_until_its_caller_ends_it() {
    Core c;
    c.opening = opening_at(1);
    iv1::Response overflow;
    overflow.mutable_subscribe()->mutable_overflow()->set_at(40);
    c.standing = {event_at(5), event_at(6), event_at(7), answered("standing", overflow)};
    c.end_after = 3;
    c.answer = [](const std::string &call, const iv1::Request &r, const Send &send) {
        if (!r.has_subscribe()) {
            iv1::Response resp;
            resp.mutable_read();
            send(answered(call, resp));
            return;
        }
        iv1::Response resp;
        resp.mutable_subscribe()->mutable_snapshot()->set_at(8);
        send(answered(call, resp));
        for (std::uint64_t n : {9, 10}) {
            iv1::CoreFrame f;
            f.set_call(call);
            f.mutable_event()->set_seq(n);
            send(f);
        }
    };
    Served s(c);
    auto a = yi::Attachment::attach(s.path, {}, 3s);
    for (std::uint64_t want : {5, 6, 7}) {
        auto got = a->standing()->next();
        REQUIRE(got && got->event().seq() == want, "the standing subscription did not deliver the event ",
                want);
    }
    auto o = a->standing()->next();
    REQUIRE(o && o->has_overflow() && o->overflow().at() == 40,
            "the standing subscription did not deliver the overflow");

    auto sub = a->subscribe(iv1::Filter());
    auto snap = sub->next();
    REQUIRE(snap && snap->snapshot().at() == 8, "the subscription did not open with its snapshot");
    for (std::uint64_t want : {9, 10}) {
        auto got = sub->next();
        REQUIRE(got && got->event().seq() == want, "the subscription did not deliver the event ", want);
    }
    sub->close();
    auto until = check::ms() + 2000;
    while (c.cancelled().size() != 1 && check::ms() < until) {
        check::sleep(10);
    }
    REQUIRE(c.cancelled().size() == 1, "ending the subscription cancelled ", c.cancelled().size(), " calls");

    failure_of([&] { a->read("unit"); });
    failure_of([&] { a->read("unit"); });
    bool ended = false;
    for (until = check::ms() + 2000; !ended && check::ms() < until;) {
        try {
            a->read("unit", {}, 500ms);
            check::sleep(10);
        } catch (const yi::Ended &) {
            ended = true;
        } catch (const std::exception &) {
            check::sleep(10);
        }
    }
    REQUIRE(ended, "after the attachment ended a read was not answered that it ended");
    bool standing_ended = false;
    try {
        a->standing()->next();
    } catch (const yi::Ended &) {
        standing_ended = true;
    }
    REQUIRE(standing_ended, "the standing subscription went on after the attachment ended");
    REQUIRE(c.attachments() == 1, "the Core saw ", c.attachments(), " attachments: something reconnected");
}

// std: yoke-sdk-cpp:the-interface-library.06
void test_confirmation_is_sent_only_when_the_author_confirms() {
    Core c;
    c.opening = opening_at(1);
    c.answer = [](const std::string &call, const iv1::Request &r, const Send &send) {
        send(answered(call, answer_of(r)));
    };
    Served s(c);
    auto a = yi::Attachment::attach(s.path, {}, 3s);
    check::sleep(200);
    REQUIRE(c.requests().empty(), "the library sent ", c.requests().size(), " requests with nothing asked");
    a->confirm(a->standing()->id(), 7);
    auto sent = c.requests();
    REQUIRE(sent.size() == 1 && sent[0].confirm().subscription() == "standing" &&
                sent[0].confirm().sequence() == 7,
            "the library sent ", sent.size(), " requests, the first ",
            sent.empty() ? std::string("none") : sent[0].ShortDebugString());
}

std::string frame(std::uint64_t sequence, std::uint64_t at, const std::string &payload) {
    std::string b(16, '\0');
    for (int i = 0; i < 8; i++) {
        b[i] = static_cast<char>((sequence >> (8 * i)) & 0xff);
        b[8 + i] = static_cast<char>((at >> (8 * i)) & 0xff);
    }
    return b + payload;
}

// std: yoke-sdk-cpp:the-interface-library.07
void test_a_streams_delivery_is_read_where_its_answer_says_it_arrives() {
    std::string socket_path = check::dir() + "/00000001.sock";
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strcpy(addr.sun_path, socket_path.c_str());
    REQUIRE(listener >= 0 && bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 &&
                listen(listener, 1) == 0,
            "the per-subscriber socket could not be made");
    std::thread delivering([listener] {
        int conn = accept(listener, nullptr, nullptr);
        if (conn < 0) {
            return;
        }
        for (auto f : {frame(1, 1001, "one"), frame(2, 1002, "two")}) {
            [[maybe_unused]] ssize_t n = send(conn, f.data(), f.size(), 0);
        }
        close(conn);
    });

    Core c;
    c.opening = opening_at(1);
    std::atomic<int> subscriptions{0};
    c.answer = [&](const std::string &call, const iv1::Request &r, const Send &send) {
        iv1::Response resp;
        if (r.has_stream_unsubscribe()) {
            resp.mutable_stream_unsubscribe();
            send(answered(call, resp));
            return;
        }
        auto *d = resp.mutable_stream_subscribe();
        switch (++subscriptions) {
        case 1:
            d->set_delivery("00000001"), d->set_flowing(true), d->set_socket(socket_path);
            send(answered(call, resp));
            break;
        case 2: {
            d->set_delivery("00000002"), d->set_flowing(true), d->set_connection(true);
            send(answered(call, resp));
            check::sleep(50);
            std::uint64_t i = 0;
            for (const char *p : {"one", "two"}) {
                iv1::CoreFrame f;
                f.set_call("00000002");
                f.mutable_delivery()->set_delivery("00000002");
                f.mutable_delivery()->set_sequence(i + 1);
                f.mutable_delivery()->set_sent_at(1001 + i);
                f.mutable_delivery()->set_payload(p);
                send(f);
                i++;
            }
            iv1::CoreFrame done;
            done.set_call("00000002");
            done.mutable_completion()->set_by(iv1::Completion::BY_CORE);
            send(done);
            break;
        }
        default:
            d->set_delivery("00000003"), d->set_connection(true);
            send(answered(call, resp));
        }
    };
    Served s(c);
    auto a = yi::Attachment::attach(s.path, {}, 3s);
    for (const char *want : {"00000001", "00000002"}) {
        auto d = a->subscribe_stream("acquire", "station.spectra");
        REQUIRE(d->id() == want && d->flowing(), "the delivery is ", d->id(), ", flowing ", d->flowing());
        std::uint64_t i = 0;
        for (const char *payload : {"one", "two"}) {
            auto f = d->next();
            REQUIRE(f && f->sequence == i + 1 && f->sent_at == 1001 + i && f->payload == payload, want,
                    "'s frame ", i, " is wrong");
            i++;
        }
        REQUIRE(!d->next(), want, " did not end");
    }
    delivering.join();
    close(listener);
    auto d = a->subscribe_stream("acquire", "station.spectra");
    d->release();
    auto sent = c.requests();
    REQUIRE(!sent.empty() && sent.back().stream_unsubscribe().delivery() == "00000003", "releasing sent ",
            sent.empty() ? std::string("nothing") : sent.back().ShortDebugString());
}

// std: yoke-sdk-cpp:the-interface-library.08
void test_closing_an_attachment_ends_it_in_order() {
    Core c;
    c.opening = opening_at(1);
    c.answer = [](const std::string &call, const iv1::Request &r, const Send &send) {
        send(answered(call, answer_of(r)));
    };
    Served s(c);
    auto a = yi::Attachment::attach(s.path, {}, 3s);
    a->read("instance");
    auto started = check::ms();
    a->close();
    auto took = check::ms() - started;
    auto until = check::ms() + 2000;
    while (c.endings().empty() && check::ms() < until) {
        check::sleep(10);
    }
    auto endings = c.endings();
    REQUIRE(endings.size() == 1 && endings[0] == "closed", "the Core read the attachment's end as ",
            endings.empty() ? std::string("nothing") : endings[0]);
    REQUIRE(took < 1000, "close returned after ", took, " ms, and not once the Core ended the attachment");
}

// std: yoke-sdk-cpp:the-interface-library.09
void test_a_panel_links_nothing_of_the_administrative_contract() {
    std::string symbols;
    FILE *nm = popen("nm -C --defined-only " YOKE_CPP_LIBRARY " 2>/dev/null", "r");
    REQUIRE(nm != nullptr, "the library's symbols cannot be listed");
    char line[4096];
    while (fgets(line, sizeof line, nm) != nullptr) {
        symbols += line;
    }
    REQUIRE(pclose(nm) == 0 && !symbols.empty(), "nm read no symbols from " YOKE_CPP_LIBRARY);
    REQUIRE(symbols.find("yoke::administrative::") == std::string::npos,
            "the library defines symbols of the administrative contract, which a panel would link");
}

} // namespace

int main() {
    return check::run({
        {"yoke-sdk-cpp:the-interface-library.01", test_a_channels_address_is_computed_or_handed},
        {"yoke-sdk-cpp:the-interface-library.02", test_attaching_surfaces_the_opening},
        {"yoke-sdk-cpp:the-interface-library.03", test_one_member_per_operation_on_one_connection},
        {"yoke-sdk-cpp:the-interface-library.04", test_a_refusal_is_the_bases_error_with_its_suspension},
        {"yoke-sdk-cpp:the-interface-library.05",
         test_a_subscription_is_delivered_in_order_until_its_caller_ends_it},
        {"yoke-sdk-cpp:the-interface-library.06", test_confirmation_is_sent_only_when_the_author_confirms},
        {"yoke-sdk-cpp:the-interface-library.07",
         test_a_streams_delivery_is_read_where_its_answer_says_it_arrives},
        {"yoke-sdk-cpp:the-interface-library.08", test_closing_an_attachment_ends_it_in_order},
        {"yoke-sdk-cpp:the-interface-library.09", test_a_panel_links_nothing_of_the_administrative_contract},
    });
}
