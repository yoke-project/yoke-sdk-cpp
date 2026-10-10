// The harness's cases: a control socket of the case's own stands in for the suite, and a plugin channel
// for the Core. The harness runs in a thread of the case's process, so that what it does to the process —
// a termination signal, its exit — is the case's to observe.
#include <atomic>
#include <csignal>
#include <cstring>
#include <memory>
#include <thread>

#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "channel.hpp"
#include "check.hpp"
#include "harness.hpp"

using json = nlohmann::json;

namespace {

// The suite's side: the socket it listens on, the harness's connection, and what is left to read.
struct Suite {
    std::string path;
    int listening = -1, conn = -1;
    std::string pending;
    std::thread harness;
    std::atomic<bool> running{false};
    std::atomic<int> status{-1};

    explicit Suite(Channel *ch) {
        path = check::dir() + "/suite.sock";
        unlink(path.c_str());
        listening = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strcpy(addr.sun_path, path.c_str());
        REQUIRE(bind(listening, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 &&
                    listen(listening, 1) == 0,
                "the suite's socket cannot be bound");
        yoke::Getenv channel_env = ch ? ch->getenv() : yoke::Getenv([](const char *) { return nullptr; });
        std::string p = path;
        running = true;
        harness = std::thread([this, p, channel_env] {
            status = harness::serve([&](const char *name) -> const char * {
                if (std::strcmp(name, "CONFORMANCE_SOCKET") == 0) {
                    return p.c_str();
                }
                if (std::strcmp(name, "YOKE_UNIT") == 0) {
                    return "harness";
                }
                return channel_env(name);
            });
            running = false;
        });
        pollfd pf{listening, POLLIN, 0};
        if (poll(&pf, 1, 5000) != 1) {
            // A harness that never connects has returned, or will not: either way the case ends here.
            harness.detach();
            REQUIRE(false, "the harness never connected");
        }
        conn = accept(listening, nullptr, nullptr);
    }

    ~Suite() {
        if (harness.joinable()) {
            if (running) {
                write_line("{\"type\":\"finish\"}");
            }
            harness.join();
        }
        close(conn);
        close(listening);
    }

    // The next line the harness wrote, parsed; null when none came within timeout_ms.
    json read(int timeout_ms = 5000) {
        for (long long until = check::ms() + timeout_ms;;) {
            auto nl = pending.find('\n');
            if (nl != std::string::npos) {
                json o = json::parse(pending.substr(0, nl), nullptr, false);
                pending.erase(0, nl + 1);
                return o;
            }
            int left = static_cast<int>(until - check::ms());
            pollfd pf{conn, POLLIN, 0};
            if (left <= 0 || poll(&pf, 1, left) != 1) {
                return nullptr;
            }
            char buf[65536];
            ssize_t n = ::read(conn, buf, sizeof buf);
            if (n <= 0) {
                return nullptr;
            }
            pending.append(buf, static_cast<std::size_t>(n));
        }
    }

    void write_line(const std::string &line) {
        std::string s = line + "\n";
        [[maybe_unused]] ssize_t n = write(conn, s.data(), s.size());
    }

    // Whether the harness exited within timeout_ms, and with what status.
    bool exited(int timeout_ms, int &got) {
        for (long long until = check::ms() + timeout_ms; running && check::ms() < until;) {
            check::sleep(20);
        }
        if (running) {
            return false;
        }
        harness.join();
        got = status;
        return true;
    }

    json result_of(const std::string &id) {
        for (json o; !(o = read()).is_null();) {
            if (o.value("type", "") == "result" && o.value("id", "") == id) {
                return o;
            }
        }
        return nullptr;
    }

    json observation() {
        for (json o; !(o = read()).is_null();) {
            if (o.value("type", "") == "observation") {
                return o;
            }
        }
        return nullptr;
    }
};

void revoke(Channel &ch, const std::string &line) {
    pluginv1::Envelope e;
    e.mutable_session()->mutable_revoked()->set_cause(
        pluginv1::SessionMessage::Revoked::CAUSE_PLUGIN_DISABLED);
    e.mutable_session()->mutable_revoked()->set_line(line);
    ch.send(e);
}

// Starts the harness against ch, and its unit with it.
void start(Suite &s, Channel &ch) {
    s.write_line(R"({"type":"directive","id":"s","verb":"start"})");
    json r = s.result_of("s");
    REQUIRE(!r.is_null() && r["value"].value("outcome", "") == "accepted",
            "the unit did not start: ", r.dump());
    REQUIRE(ch.await(pluginv1::Envelope::kSession, 0, 5000) >= 0, "the Session was never opened");
}

// std: yoke-sdk-cpp:the-harness.01
void test_hello_first_with_what_the_library_declares() {
    Suite s(nullptr);
    json hello = s.read();
    REQUIRE(!hello.is_null() && hello.value("type", "") == "hello" &&
                hello.value("contract", "") == "plugin" && hello.value("language", "") == "cpp" &&
                hello.value("sdk", "") == yoke::sdk_line &&
                hello.value("version", 0) == yoke::plugin_contract() && hello.value("unit", "") == "harness",
            "the first line is ", hello.dump());
}

// std: yoke-sdk-cpp:the-harness.02
void test_describe_answers_with_the_manifest_the_library_generates() {
    Suite s(nullptr);
    s.write_line(R"({"type":"directive","id":"d-1","verb":"describe"})");
    json r = s.result_of("d-1");
    REQUIRE(!r.is_null() && r["value"].value("manifest", "") == harness::declaration().manifest(),
            "describe answered ", r.dump());
}

// std: yoke-sdk-cpp:the-harness.03
void test_start_reports_what_admission_answered_and_a_refusal_as_its_code() {
    Terms restricted;
    restricted.outcome = pluginv1::RegisterResponse::OUTCOME_ACCEPTED_WITH_RESTRICTIONS;
    restricted.granted.add_streams("conformance.data");
    restricted.withheld.add_streams("conformance.frames");
    {
        Channel ch(check::dir(), restricted);
        Suite s(&ch);
        s.write_line(R"({"type":"directive","id":"s","verb":"start"})");
        json r = s.result_of("s");
        REQUIRE(!r.is_null() && r["value"].value("outcome", "") == "accepted with restrictions",
                "start answered ", r.dump());
        REQUIRE(r["value"]["granted"]["streams"] == json::array({"conformance.data"}) &&
                    r["value"]["withheld"]["streams"] == json::array({"conformance.frames"}),
                "the granted and withheld items are not named: ", r.dump());
    }
    // A second harness, against a channel that refuses, in a directory of its own.
    Terms refused;
    refused.outcome = pluginv1::RegisterResponse::OUTCOME_REFUSED;
    refused.stage = pluginv1::STAGE_AUTHENTICATION;
    refused.code = "admission.auth.consumed";
    refused.message = "the token was already used";
    std::string second = check::dir() + "/refused";
    mkdir(second.c_str(), 0700);
    Channel ch(second, refused);
    Suite s(&ch);
    s.write_line(R"({"type":"directive","id":"s","verb":"start"})");
    json r = s.result_of("s");
    REQUIRE(!r.is_null() && r.value("refusal", "") == "admission.auth.consumed" &&
                r["value"].value("stage", "") == "authentication",
            "a refused start answered ", r.dump());
    REQUIRE(r.dump().find("already used") == std::string::npos, "the library's message reached the suite");
}

// std: yoke-sdk-cpp:the-harness.04
void test_a_verb_it_does_not_know_is_reported_as_unrecognised() {
    Suite s(nullptr);
    s.write_line(R"({"type":"directive","id":"u-1","verb":"subscribe"})");
    json r = s.result_of("u-1");
    REQUIRE(!r.is_null() && r.value("unrecognised", false), "subscribe answered ", r.dump());
}

// std: yoke-sdk-cpp:the-harness.05
void test_what_the_library_surfaces_is_observed_in_order_and_the_end_ends_it() {
    Channel ch(check::dir());
    Suite s(&ch);
    start(s, ch);
    pluginv1::Envelope command;
    command.mutable_control()->mutable_command()->set_type("calibrate");
    ch.send(command);
    revoke(ch, "the operator disabled the plugin");
    json first = s.observation(), second = s.observation();
    REQUIRE(!first.is_null() && first.value("kind", "") == "command" &&
                first["fields"].value("type", "") == "calibrate",
            "the first observation is ", first.dump());
    REQUIRE(!second.is_null() && second.value("kind", "") == "session-ended" &&
                second["fields"].value("cause", "") == "plugin disabled" &&
                !second["fields"].value("closed", true),
            "the second observation is ", second.dump());
    int status = -1;
    REQUIRE(s.exited(3000, status) && status == 0, "the harness did not exit with 0 after the end");
}

// std: yoke-sdk-cpp:the-harness.06
void test_finish_ends_the_harness_which_holds_no_wire() {
    Channel ch(check::dir());
    Suite s(&ch);
    start(s, ch);
    s.write_line(R"({"type":"finish"})");
    int status = -1;
    REQUIRE(s.exited(3000, status) && status == 0, "the harness did not exit with 0 on finish");
    std::string code = check::read(SOURCE_DIR "/harness/harness.cpp"),
                header = check::read(SOURCE_DIR "/harness/harness.hpp"),
                build = check::read(SOURCE_DIR "/CMakeLists.txt");
    for (const char *wire : {".pb.h", "grpcpp/", "google/protobuf"}) {
        REQUIRE(code.find(wire) == std::string::npos && header.find(wire) == std::string::npos,
                "the harness includes ", wire);
    }
    auto linked = build.find("target_link_libraries(yoke-cpp-plugin-harness");
    REQUIRE(linked != std::string::npos, "the harness's links cannot be read");
    std::string links = build.substr(linked, build.find(')', linked) - linked);
    REQUIRE(links.find("GRPC") == std::string::npos && links.find("PROTOBUF") == std::string::npos,
            "the harness links a wire of its own");
}

// std: yoke-sdk-cpp:the-harness.07
void test_asked_to_stop_it_reports_the_end_first_then_leaves() {
    Channel ch(check::dir());
    Suite s(&ch);
    start(s, ch);
    kill(getpid(), SIGTERM);
    check::sleep(200);
    revoke(ch, "the Core is stopping");
    json ended = s.observation();
    REQUIRE(!ended.is_null() && ended.value("kind", "") == "session-ended",
            "the end was not reported: ", ended.dump());
    int status = -1;
    REQUIRE(s.exited(3000, status) && status == 0, "the harness did not exit with 0");
}

// std: yoke-sdk-cpp:the-harness.08
void test_the_suite_is_the_published_pair_authenticated_and_never_built() {
    std::string script = check::read(SOURCE_DIR "/ci/conformance.sh");
    std::string workflow = check::read(SOURCE_DIR "/.github/workflows/verify.yml");
    REQUIRE(!script.empty(), "no ci/conformance.sh");
    REQUIRE(script.find("yoke-conformance-") != std::string::npos &&
                script.find("releases/download") != std::string::npos,
            "the script does not download the published archive");
    REQUIRE(script.find("manifest.jsonl") != std::string::npos &&
                script.find("sha256sum") != std::string::npos,
            "the script does not authenticate it by the manifest");
    for (const char *build : {"go install", "go build", "go run"}) {
        REQUIRE(script.find(build) == std::string::npos, "the script builds yoke with ", build);
    }
    auto test = workflow.find("run: just test"), conformance = workflow.find("run: ci/conformance.sh");
    REQUIRE(test != std::string::npos && conformance != std::string::npos && conformance > test,
            "the workflow does not run the suite after just test");
}

// std: yoke-sdk-cpp:the-harness.09
void test_a_question_is_observed_with_the_bytes_it_carries() {
    Channel ch(check::dir());
    Suite s(&ch);
    start(s, ch);
    pluginv1::Envelope e;
    e.mutable_query()->mutable_question()->set_type("status");
    e.mutable_query()->mutable_question()->set_payload("how are you");
    ch.send(e);
    json q = s.observation();
    REQUIRE(!q.is_null() && q.value("kind", "") == "question" && q["fields"].value("type", "") == "status" &&
                q["fields"].value("payload", "") == "how are you" && !q["fields"].value("id", "").empty(),
            "the question was observed as ", q.dump());
}

// std: yoke-sdk-cpp:the-harness.10
void test_it_declares_a_stream_on_each_transport_each_governed() {
    const auto &d = harness::declaration();
    const yoke::plugin::Stream *ordered = nullptr, *framed = nullptr;
    for (const auto &s : d.streams) {
        if (!s.tolerates_loss && !s.tolerates_reorder) {
            ordered = &s;
        } else if (s.tolerates_loss) {
            framed = &s;
        }
    }
    REQUIRE(ordered && framed, "it does not declare a stream on each transport");
    for (const auto *s : {ordered, framed}) {
        bool governed = false;
        for (const auto &c : d.capabilities) {
            governed = governed || (c.governs.kind == yoke::plugin::Governs::stream && c.governs.id == s->id);
        }
        REQUIRE(governed, "the stream ", s->id, " is governed by no capability");
    }
}

} // namespace

int main() {
    return check::run({
        {"yoke-sdk-cpp:the-harness.01", test_hello_first_with_what_the_library_declares},
        {"yoke-sdk-cpp:the-harness.02", test_describe_answers_with_the_manifest_the_library_generates},
        {"yoke-sdk-cpp:the-harness.03", test_start_reports_what_admission_answered_and_a_refusal_as_its_code},
        {"yoke-sdk-cpp:the-harness.04", test_a_verb_it_does_not_know_is_reported_as_unrecognised},
        {"yoke-sdk-cpp:the-harness.05",
         test_what_the_library_surfaces_is_observed_in_order_and_the_end_ends_it},
        {"yoke-sdk-cpp:the-harness.06", test_finish_ends_the_harness_which_holds_no_wire},
        {"yoke-sdk-cpp:the-harness.07", test_asked_to_stop_it_reports_the_end_first_then_leaves},
        {"yoke-sdk-cpp:the-harness.08", test_the_suite_is_the_published_pair_authenticated_and_never_built},
        {"yoke-sdk-cpp:the-harness.09", test_a_question_is_observed_with_the_bytes_it_carries},
        {"yoke-sdk-cpp:the-harness.10", test_it_declares_a_stream_on_each_transport_each_governed},
    });
}
