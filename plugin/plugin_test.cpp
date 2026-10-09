#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "channel.hpp"
#include "check.hpp"
#include "yoke/plugin.hpp"

using namespace std::chrono_literals;
using yoke::plugin::Unit;
namespace yp = yoke::plugin;

namespace {

// The declaration the cases start a unit with.
const yp::Declaration station = {
    "com.yoke.station.acquire",
    {"device:instrument"},
    {{"station.spectra", false, false}, {"station.preview", true, true}},
    {"calibrate"},
    {"head-status"},
    {"calibration.drift"},
    {
        {"acquire.spectra", {yp::Governs::stream, "station.spectra"}},
        {"acquire.preview", {yp::Governs::stream, "station.preview"}},
        {"acquire.calibrate", {yp::Governs::command, "calibrate"}},
        {"acquire.status", {yp::Governs::query, "head-status"}},
        {"acquire.drift", {yp::Governs::occurrence, "calibration.drift"}},
    },
};

// A channel on terms, and a unit started against it whose Session is open.
struct Opened {
    std::unique_ptr<Channel> ch;
    std::unique_ptr<Unit> unit;

    explicit Opened(Terms terms = {}) : ch(std::make_unique<Channel>(check::dir(), std::move(terms))) {
        unit = Unit::start(station, ch->getenv());
        REQUIRE(ch->await(pluginv1::Envelope::kSession, 0, 5000) >= 0, "the Session was never opened");
    }
    ~Opened() {
        unit.reset();
        ch.reset();
    }
};

// The next event, which must be of the type given.
template <typename T> T next_of(Unit &u) {
    std::optional<yp::Event> ev = u.next(5000ms);
    REQUIRE(ev.has_value(), "no event came");
    REQUIRE(std::holds_alternative<T>(*ev), "an event of another kind came, of index ", ev->index());
    return std::get<T>(*ev);
}

// The header with its comments removed: what an author can write against.
std::string header_code() {
    std::string text = check::read(SOURCE_DIR "/include/yoke/plugin.hpp");
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "/*") == 0) {
            std::size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 2;
        } else if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n') {
                i++;
            }
        } else {
            out += text[i++];
        }
    }
    return out;
}

// The body of the struct a header declares as `struct name {`.
std::string struct_body(const std::string &code, const std::string &name) {
    std::size_t at = code.find("struct " + name + " {");
    if (at == std::string::npos) {
        return {};
    }
    std::size_t open = code.find('{', at);
    std::size_t close = code.find("};", open);
    return code.substr(open + 1, close - open - 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

template <typename List> std::vector<std::string> strings(const List &list) {
    return std::vector<std::string>(list.begin(), list.end());
}

// std: yoke-sdk-cpp:the-plugin-library.01
void test_a_declaration_generates_the_manifest() {
    std::string want = "manifest: 1\n"
                       "id: \"com.yoke.station.acquire\"\n"
                       "protocol: " +
                       std::to_string(yoke::plugin_contract()) +
                       "\n"
                       "needs:\n"
                       "  - \"device:instrument\"\n"
                       "streams:\n"
                       "  - id: \"station.spectra\"\n"
                       "  - id: \"station.preview\"\n"
                       "    tolerates_loss: true\n"
                       "    tolerates_reorder: true\n"
                       "commands:\n"
                       "  - id: \"calibrate\"\n"
                       "queries:\n"
                       "  - id: \"head-status\"\n"
                       "occurrences:\n"
                       "  - id: \"calibration.drift\"\n"
                       "capabilities:\n"
                       "  - name: \"acquire.spectra\"\n"
                       "    governs:\n"
                       "      stream: \"station.spectra\"\n"
                       "  - name: \"acquire.preview\"\n"
                       "    governs:\n"
                       "      stream: \"station.preview\"\n"
                       "  - name: \"acquire.calibrate\"\n"
                       "    governs:\n"
                       "      command: \"calibrate\"\n"
                       "  - name: \"acquire.status\"\n"
                       "    governs:\n"
                       "      query: \"head-status\"\n"
                       "  - name: \"acquire.drift\"\n"
                       "    governs:\n"
                       "      occurrence: \"calibration.drift\"\n";
    std::string got = station.manifest();
    REQUIRE(got == want, "the Manifest is:\n", got);
}

// std: yoke-sdk-cpp:the-plugin-library.02
void test_nothing_the_model_does_not_have_can_be_declared() {
    std::string code = header_code();
    REQUIRE(!code.empty(), "the plugin library's header cannot be read");
    for (const char *type : {"Declaration", "Stream", "Capability", "Object"}) {
        std::string body = lower(struct_body(code, type));
        REQUIRE(!body.empty(), "the header declares no struct ", type);
        for (const char *word : {"endpoint", "autostart", "digest", "description"}) {
            REQUIRE(body.find(word) == std::string::npos, type, " has a field for ", word);
        }
    }
}

// std: yoke-sdk-cpp:the-plugin-library.03
void test_the_registration_claims_what_the_manifest_declares() {
    Opened o;
    auto r = o.ch->request();
    REQUIRE(r.has_value(), "no registration arrived");
    REQUIRE(r->plugin() == "com.yoke.station.acquire", "the plugin is ", r->plugin());
    REQUIRE(r->unit() == "acquire", "the unit is ", r->unit());
    REQUIRE(r->token() == "t-0123", "the token is ", r->token());
    REQUIRE(static_cast<int>(r->protocol()) == yoke::plugin_contract(), "the protocol is ", r->protocol());
    REQUIRE(r->language() == "cpp", "the language is ", r->language());
    REQUIRE(r->sdk_line() == yoke::sdk_line, "the SDK line is ", r->sdk_line());
    const auto &d = r->declared();
    REQUIRE(strings(d.capabilities()) ==
                std::vector<std::string>({"acquire.spectra", "acquire.preview", "acquire.calibrate",
                                          "acquire.status", "acquire.drift"}),
            "the capabilities differ");
    REQUIRE(strings(d.streams()) == std::vector<std::string>({"station.spectra", "station.preview"}),
            "the streams differ");
    REQUIRE(strings(d.commands()) == station.commands, "the commands differ");
    REQUIRE(strings(d.queries()) == station.queries, "the queries differ");
    REQUIRE(strings(d.occurrences()) == station.occurrences, "the occurrences differ");
    REQUIRE(pluginv1::RegisterRequest::descriptor()->FindFieldByName("incarnation") == nullptr,
            "the request has an incarnation");
}

// std: yoke-sdk-cpp:the-plugin-library.04
void test_the_socket_is_bound_before_registering() {
    Opened o;
    REQUIRE(o.ch->bound_at_registration(), "nothing was bound at ", o.ch->bind(),
            " when the registration arrived");
}

// std: yoke-sdk-cpp:the-plugin-library.05
void test_a_refusal_is_surfaced_and_never_retried() {
    Terms terms;
    terms.outcome = pluginv1::RegisterResponse::OUTCOME_REFUSED;
    terms.stage = pluginv1::STAGE_AUTHENTICATION;
    terms.code = "admission.auth.consumed";
    terms.message = "the token was already used";
    Channel ch(check::dir(), terms);
    try {
        auto u = Unit::start(station, ch.getenv());
        REQUIRE(false, "the unit started on a refused registration");
    } catch (const yoke::Refusal &r) {
        REQUIRE(r.code() == "admission.auth.consumed", "the refusal's code is ", r.code());
        REQUIRE(r.stage() == "authentication", "the refusal's stage is ", r.stage());
    }
    check::sleep(300);
    REQUIRE(ch.registrations() == 1, ch.registrations(), " registrations arrived");
    REQUIRE(ch.sessions() == 0, ch.sessions(), " Sessions were opened");
}

// std: yoke-sdk-cpp:the-plugin-library.06
void test_an_acceptance_with_restrictions_names_what_was_withheld() {
    Terms terms;
    terms.outcome = pluginv1::RegisterResponse::OUTCOME_ACCEPTED_WITH_RESTRICTIONS;
    terms.granted.add_streams("station.spectra");
    terms.withheld.add_streams("station.preview");
    terms.withheld.add_occurrences("calibration.drift");
    Opened o(terms);
    const yp::Admission &a = o.unit->admission();
    REQUIRE(a.restricted, "the unit does not say it was admitted with restrictions");
    REQUIRE(a.granted.streams == std::vector<std::string>({"station.spectra"}),
            "the granted streams are not station.spectra alone");
    REQUIRE(a.withheld.streams == std::vector<std::string>({"station.preview"}),
            "the withheld stream is not named");
    REQUIRE(a.withheld.occurrences == std::vector<std::string>({"calibration.drift"}),
            "the withheld occurrence is not named");
}

// std: yoke-sdk-cpp:the-plugin-library.07
void test_the_session_opens_and_beats_on_the_cores_terms() {
    Terms terms;
    terms.interval_ms = 100;
    Opened o(terms);
    o.unit->health(30, "starting");
    check::sleep(450);
    pluginv1::Envelope first = o.ch->arrival(0);
    REQUIRE(first.has_session() && first.session().has_open(), "the Session's first envelope is not an OPEN");
    REQUIRE(first.session_id() == "s-1", "the OPEN carries the Session ", first.session_id());
    int beats = -1; // the author's own report is not a beat
    long long last = -1;
    for (std::size_t i = 1; i < o.ch->count(); i++) {
        long long at = 0;
        pluginv1::Envelope e = o.ch->arrival(i, &at);
        if (!e.has_health()) {
            continue;
        }
        REQUIRE(last < 0 || at - last >= 50, "two health reports arrived ", at - last, " ms apart");
        last = at;
        beats++;
    }
    REQUIRE(beats >= 3, std::max(beats, 0), " heartbeats followed the OPEN");
    REQUIRE(lower(header_code()).find("interval") == std::string::npos,
            "an author is given a way to choose the interval");
}

void revoke_session(Channel &ch, pluginv1::SessionMessage::Revoked::Cause cause, const std::string &line) {
    pluginv1::Envelope e;
    e.mutable_session()->mutable_revoked()->set_cause(cause);
    e.mutable_session()->mutable_revoked()->set_line(line);
    ch.send(e);
}

// std: yoke-sdk-cpp:the-plugin-library.08
void test_the_end_of_a_session_is_surfaced_and_nothing_reconnects() {
    Opened o;
    revoke_session(*o.ch, pluginv1::SessionMessage::Revoked::CAUSE_PLUGIN_DISABLED,
                   "the operator disabled the plugin");
    auto end = next_of<yp::Ended>(*o.unit);
    REQUIRE(!end.closed, "the end is surfaced as a close the unit made");
    REQUIRE(end.cause == "plugin disabled", "the cause is ", end.cause);
    REQUIRE(end.line == "the operator disabled the plugin", "the line is ", end.line);
    REQUIRE(o.unit->done(), "the unit does not report itself done");
    REQUIRE(!o.unit->next(100ms).has_value(), "something followed the end");
    REQUIRE(!o.unit->next().has_value(), "something followed the end");
    check::sleep(500);
    REQUIRE(o.ch->registrations() == 1, o.ch->registrations(), " registrations arrived");
    REQUIRE(o.ch->sessions() == 1, o.ch->sessions(), " Sessions were opened");
}

// std: yoke-sdk-cpp:the-plugin-library.09
void test_an_orderly_close_is_the_units() {
    Opened o;
    o.unit->close();
    long i = o.ch->await(pluginv1::Envelope::kSession, 1, 3000);
    REQUIRE(i >= 0, "no session message followed the OPEN");
    REQUIRE(o.ch->arrival(static_cast<std::size_t>(i)).session().has_close(),
            "the session message that followed is not a CLOSE");
    auto end = next_of<yp::Ended>(*o.unit);
    REQUIRE(end.closed, "the end is not surfaced as a close the unit made");
}

std::string send_command(Channel &ch, const std::string &type) {
    pluginv1::Envelope e;
    e.mutable_control()->mutable_command()->set_type(type);
    return ch.send(e);
}

std::string send_question(Channel &ch, const std::string &type) {
    pluginv1::Envelope e;
    e.mutable_query()->mutable_question()->set_type(type);
    return ch.send(e);
}

// std: yoke-sdk-cpp:the-plugin-library.10
void test_what_the_core_sends_is_surfaced_and_answers_are_correlated() {
    Opened o;
    std::string command_id = send_command(*o.ch, "calibrate");
    std::string question_id = send_question(*o.ch, "head-status");
    auto command = next_of<yp::Command>(*o.unit);
    REQUIRE(command.type == "calibrate" && command.id == command_id, "the command surfaced is ", command.type,
            ", ", command.id);
    auto question = next_of<yp::Question>(*o.unit);
    REQUIRE(question.type == "head-status" && question.id == question_id, "the question surfaced is ",
            question.type, ", ", question.id);
    o.unit->ack(command, yp::Outcome::done, "calibrated");
    o.unit->answer(question, "nominal");
    long i = o.ch->await(pluginv1::Envelope::kAck, 0, 3000);
    REQUIRE(i >= 0, "no acknowledgement arrived");
    REQUIRE(o.ch->arrival(static_cast<std::size_t>(i)).correlation_id() == command_id,
            "the acknowledgement is not correlated to the command");
    i = o.ch->await(pluginv1::Envelope::kQuery, 0, 3000);
    REQUIRE(i >= 0, "no answer arrived");
    pluginv1::Envelope answer = o.ch->arrival(static_cast<std::size_t>(i));
    REQUIRE(answer.correlation_id() == question_id, "the answer is not correlated to the question");
    REQUIRE(answer.query().has_answer() && answer.query().answer().payload() == "nominal",
            "the answer does not carry the payload");
}

// std: yoke-sdk-cpp:the-plugin-library.11
void test_an_occurrence_carries_the_authors_severity_or_is_refused() {
    Opened o;
    bool refused = false;
    try {
        o.unit->report("calibration.drift", yp::Severity(), "drifted");
    } catch (const std::runtime_error &) {
        refused = true;
    }
    REQUIRE(refused, "an occurrence with no severity was reported");
    check::sleep(200);
    REQUIRE(o.ch->await(pluginv1::Envelope::kEvent, 0, 0) < 0,
            "an event reached the channel with no severity stated");
    o.unit->report("calibration.drift", yp::Severity::of(40), "drifted");
    long i = o.ch->await(pluginv1::Envelope::kEvent, 0, 3000);
    REQUIRE(i >= 0, "no event arrived");
    pluginv1::Event e = o.ch->arrival(static_cast<std::size_t>(i)).event();
    REQUIRE(e.severity() == 40 && e.occurrence() == "calibration.drift", "the event is ", e.occurrence(),
            " with severity ", e.severity());
}

// How many entries the case's directory holds.
int entries() {
    int n = 0;
    for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator(check::dir())) {
        n++;
    }
    return n;
}

// The refusal an emission meets, or an empty code if it met none.
std::string emission_refused(Unit &u, const std::string &stream) {
    try {
        u.emit(stream, "x");
    } catch (const yoke::Refusal &r) {
        return r.code();
    }
    return {};
}

// std: yoke-sdk-cpp:the-plugin-library.12
void test_nothing_is_emitted_on_a_stream_not_activated() {
    Opened o;
    int before = entries();
    std::string code = emission_refused(*o.unit, "station.spectra");
    REQUIRE(code == "stream.inactive", "the emission was met with '", code, "'");
    REQUIRE(entries() == before, "something was created beside the channel");
    check::sleep(200);
    REQUIRE(o.ch->await(pluginv1::Envelope::kData, 0, 0) < 0, "data reached the channel");
}

// The acknowledgement correlated to id, searching from *from.
std::optional<pluginv1::Ack> ack_of(Channel &ch, const std::string &id, std::size_t *from) {
    for (;;) {
        long i = ch.await(pluginv1::Envelope::kAck, *from, 3000);
        if (i < 0) {
            return std::nullopt;
        }
        *from = static_cast<std::size_t>(i) + 1;
        pluginv1::Envelope e = ch.arrival(static_cast<std::size_t>(i));
        if (e.correlation_id() == id) {
            return e.ack();
        }
    }
}

// std: yoke-sdk-cpp:the-plugin-library.13
void test_every_family_a_unit_originates_has_an_act() {
    Opened o;
    std::string command_id = send_command(*o.ch, "calibrate");
    std::string question_id = send_question(*o.ch, "head-status");
    auto command = next_of<yp::Command>(*o.unit);
    auto question = next_of<yp::Question>(*o.unit);
    o.unit->ack(command, yp::Outcome::accepted);
    o.unit->ack(command, yp::Outcome::done, "calibrated");
    o.unit->fail(question.id, "head.unreachable", "the head did not answer");
    std::size_t from = 0;
    auto a = ack_of(*o.ch, command_id, &from);
    REQUIRE(a && a->outcome() == pluginv1::Ack::OUTCOME_ACCEPTED,
            "the first acknowledgement is not `accepted`");
    a = ack_of(*o.ch, command_id, &from);
    REQUIRE(a && a->outcome() == pluginv1::Ack::OUTCOME_DONE, "the second acknowledgement is not `done`");
    long i = o.ch->await(pluginv1::Envelope::kError, 0, 3000);
    REQUIRE(i >= 0, "no error arrived");
    pluginv1::Envelope e = o.ch->arrival(static_cast<std::size_t>(i));
    REQUIRE(e.correlation_id() == question_id, "the error is not correlated to the question");
    REQUIRE(e.error().code() == "head.unreachable" && e.error().message() == "the head did not answer",
            "the error carries ", e.error().code(), ", ", e.error().message());
}

// std: yoke-sdk-cpp:the-plugin-library.14
void test_a_beat_repeats_the_authors_last_report() {
    Terms terms;
    terms.interval_ms = 100;
    Opened o(terms);
    check::sleep(350);
    REQUIRE(o.ch->await(pluginv1::Envelope::kHealth, 0, 0) < 0,
            "a health report reached the channel before the author's first");
    std::size_t first = o.ch->count();
    o.unit->health(40, "warming");
    check::sleep(350);
    std::size_t second = o.ch->count();
    o.unit->health(10, "cold");
    check::sleep(350);
    int warm = 0, cold = 0;
    bool seen_cold = false;
    for (std::size_t i = first; i < o.ch->count(); i++) {
        pluginv1::Envelope e = o.ch->arrival(i);
        if (!e.has_health()) {
            continue;
        }
        bool is_warm = e.health().grade() == 40 && e.health().line() == "warming";
        bool is_cold = e.health().grade() == 10 && e.health().line() == "cold";
        REQUIRE(is_warm || is_cold, "a report carries ", e.health().grade(), " and ", e.health().line(),
                ", which the author did not state");
        REQUIRE(!(is_warm && seen_cold), "40 was repeated after the author reported 10");
        if (is_cold) {
            seen_cold = true;
            cold++;
        } else if (i < second) {
            warm++;
        }
    }
    // Each count includes the author's own report.
    REQUIRE(warm >= 3, warm - 1, " beats repeated 40");
    REQUIRE(cold >= 3, cold - 1, " beats repeated 10");
}

// A socket the case listens on, closed with the case.
struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int f) : fd(f) {}
    Fd(Fd &&o) noexcept : fd(o.fd) { o.fd = -1; }
    Fd &operator=(Fd &&o) noexcept {
        std::swap(fd, o.fd);
        return *this;
    }
    ~Fd() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

Fd listen_at(const std::string &path, int type) {
    Fd s(::socket(AF_UNIX, type, 0));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
    REQUIRE(::bind(s.fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0, "the case cannot bind ",
            path);
    REQUIRE(type != SOCK_SEQPACKET || ::listen(s.fd, 4) == 0, "the case cannot listen at ", path);
    return s;
}

bool readable(int fd, int ms) {
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, ms) == 1;
}

std::string activate(Channel &ch, const std::string &stream, pluginv1::Control::Activate::Transport transport,
                     const std::string &address) {
    pluginv1::Envelope e;
    auto *a = e.mutable_control()->mutable_activate();
    a->set_stream(stream);
    a->set_transport(transport);
    a->set_address(address);
    return ch.send(e);
}

// The transports of cases 15 to 17: a packet socket and a datagram socket the case listens on.
struct Transports {
    std::string ordered = check::dir() + "/ordered.sock";
    std::string framed = check::dir() + "/framed.sock";
    std::string nothing = check::dir() + "/nothing.sock";
    Fd listener = listen_at(ordered, SOCK_SEQPACKET);
    Fd datagrams = listen_at(framed, SOCK_DGRAM);
    Fd conn;

    void accept() {
        REQUIRE(readable(listener.fd, 3000), "the library did not connect to the packet socket");
        conn = Fd(::accept(listener.fd, nullptr, nullptr));
    }
};

constexpr auto ordered = pluginv1::Control::Activate::TRANSPORT_ORDERED;
constexpr auto framed = pluginv1::Control::Activate::TRANSPORT_FRAMED;

// std: yoke-sdk-cpp:the-plugin-library.15
void test_an_activation_connects_to_its_transport_and_is_acknowledged() {
    Opened o;
    Transports t;
    std::string ordered_id = activate(*o.ch, "station.spectra", ordered, t.ordered);
    std::string framed_id = activate(*o.ch, "station.preview", framed, t.framed);
    std::string nothing_id = activate(*o.ch, "station.raw", ordered, t.nothing);
    t.accept();
    std::size_t from = 0;
    auto a = ack_of(*o.ch, ordered_id, &from);
    REQUIRE(a && a->outcome() == pluginv1::Ack::OUTCOME_DONE,
            "the ordered activation is not acknowledged as done");
    from = 0;
    a = ack_of(*o.ch, framed_id, &from);
    REQUIRE(a && a->outcome() == pluginv1::Ack::OUTCOME_DONE,
            "the framed activation is not acknowledged as done");
    from = 0;
    a = ack_of(*o.ch, nothing_id, &from);
    REQUIRE(a && a->outcome() == pluginv1::Ack::OUTCOME_FAILED && !a->line().empty(),
            "the activation nothing listens for is not acknowledged as failed, with a line");
    auto first = next_of<yp::Activated>(*o.unit);
    REQUIRE(first.stream == "station.spectra" && first.transport == "ordered" && first.address == t.ordered,
            "the author was handed ", first.stream, " on ", first.transport, " at ", first.address);
    auto second = next_of<yp::Activated>(*o.unit);
    REQUIRE(second.stream == "station.preview" && second.transport == "framed", "the author was handed ",
            second.stream, " on ", second.transport);
    REQUIRE(!o.unit->next(200ms).has_value(), "the failed activation was handed to the author");
    REQUIRE(emission_refused(*o.unit, "station.raw") == "stream.inactive",
            "the stream whose activation failed is not inactive");
}

unsigned long long le64(const unsigned char *b) {
    unsigned long long v = 0;
    for (int i = 7; i >= 0; i--) {
        v = v << 8 | b[i];
    }
    return v;
}

// std: yoke-sdk-cpp:the-plugin-library.16
void test_emit_writes_envelopes_and_frames_numbered_from_one() {
    Opened o;
    Transports t;
    activate(*o.ch, "station.spectra", ordered, t.ordered);
    activate(*o.ch, "station.preview", framed, t.framed);
    next_of<yp::Activated>(*o.unit);
    next_of<yp::Activated>(*o.unit);
    t.accept();
    const std::string payloads[] = {"one", "two", "three"};
    for (const auto &p : payloads) {
        o.unit->emit("station.spectra", p);
        o.unit->emit("station.preview", p);
    }
    for (unsigned i = 0; i < 3; i++) {
        unsigned char packet[4096];
        REQUIRE(readable(t.conn.fd, 3000), "packet ", i + 1, " did not arrive");
        ssize_t n = ::recv(t.conn.fd, packet, sizeof packet, 0);
        REQUIRE(n > 0, "packet ", i + 1, " could not be read");
        pluginv1::Envelope e;
        REQUIRE(e.ParseFromArray(packet, static_cast<int>(n)), "packet ", i + 1, " is not an envelope");
        REQUIRE(e.has_data() && e.data().sequence() == i + 1 && !e.message_id().empty() &&
                    e.session_id() == "s-1" && e.sent_at_unix_nano() > 0 && e.data().payload() == payloads[i],
                "packet ", i + 1, " is not data envelope ", i + 1,
                " with its header and the payload unchanged");

        REQUIRE(readable(t.datagrams.fd, 3000), "datagram ", i + 1, " did not arrive");
        n = ::recv(t.datagrams.fd, packet, sizeof packet, 0);
        REQUIRE(n == static_cast<ssize_t>(16 + payloads[i].size()), "datagram ", i + 1, " has ", n, " bytes");
        REQUIRE(le64(packet) == i + 1, "datagram ", i + 1, " carries the sequence ", le64(packet));
        REQUIRE(le64(packet + 8) > 0, "datagram ", i + 1, " carries no clock");
        REQUIRE(std::string(reinterpret_cast<char *>(packet) + 16, payloads[i].size()) == payloads[i],
                "datagram ", i + 1, "'s payload changed");
    }
    REQUIRE(o.ch->await(pluginv1::Envelope::kData, 0, 0) < 0, "data reached the Session");
}

// std: yoke-sdk-cpp:the-plugin-library.17
void test_a_stop_closes_the_transport_and_emit_is_refused_after_it() {
    Opened o;
    Transports t;
    activate(*o.ch, "station.spectra", ordered, t.ordered);
    next_of<yp::Activated>(*o.unit);
    t.accept();
    pluginv1::Envelope e;
    e.mutable_control()->mutable_stop()->set_stream("station.spectra");
    std::string stop_id = o.ch->send(e);
    auto stop = next_of<yp::Stopped>(*o.unit);
    REQUIRE(stop.stream == "station.spectra", "the author was handed the stop of ", stop.stream);
    std::size_t from = 0;
    auto a = ack_of(*o.ch, stop_id, &from);
    REQUIRE(a && a->outcome() == pluginv1::Ack::OUTCOME_DONE, "the stop is not acknowledged as done");
    unsigned char b[64];
    REQUIRE(readable(t.conn.fd, 3000) && ::recv(t.conn.fd, b, sizeof b, 0) == 0,
            "the library did not close its connection");
    REQUIRE(emission_refused(*o.unit, "station.spectra") == "stream.inactive",
            "an emission after the stop was not refused with stream.inactive");
}

} // namespace

int main() {
    return check::run({
        {"yoke-sdk-cpp:the-plugin-library.01", test_a_declaration_generates_the_manifest},
        {"yoke-sdk-cpp:the-plugin-library.02", test_nothing_the_model_does_not_have_can_be_declared},
        {"yoke-sdk-cpp:the-plugin-library.03", test_the_registration_claims_what_the_manifest_declares},
        {"yoke-sdk-cpp:the-plugin-library.04", test_the_socket_is_bound_before_registering},
        {"yoke-sdk-cpp:the-plugin-library.05", test_a_refusal_is_surfaced_and_never_retried},
        {"yoke-sdk-cpp:the-plugin-library.06", test_an_acceptance_with_restrictions_names_what_was_withheld},
        {"yoke-sdk-cpp:the-plugin-library.07", test_the_session_opens_and_beats_on_the_cores_terms},
        {"yoke-sdk-cpp:the-plugin-library.08", test_the_end_of_a_session_is_surfaced_and_nothing_reconnects},
        {"yoke-sdk-cpp:the-plugin-library.09", test_an_orderly_close_is_the_units},
        {"yoke-sdk-cpp:the-plugin-library.10",
         test_what_the_core_sends_is_surfaced_and_answers_are_correlated},
        {"yoke-sdk-cpp:the-plugin-library.11", test_an_occurrence_carries_the_authors_severity_or_is_refused},
        {"yoke-sdk-cpp:the-plugin-library.12", test_nothing_is_emitted_on_a_stream_not_activated},
        {"yoke-sdk-cpp:the-plugin-library.13", test_every_family_a_unit_originates_has_an_act},
        {"yoke-sdk-cpp:the-plugin-library.14", test_a_beat_repeats_the_authors_last_report},
        {"yoke-sdk-cpp:the-plugin-library.15",
         test_an_activation_connects_to_its_transport_and_is_acknowledged},
        {"yoke-sdk-cpp:the-plugin-library.16", test_emit_writes_envelopes_and_frames_numbered_from_one},
        {"yoke-sdk-cpp:the-plugin-library.17", test_a_stop_closes_the_transport_and_emit_is_refused_after_it},
    });
}
