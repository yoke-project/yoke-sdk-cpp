// The interface harness's cases: a control socket of the case's own stands in for the suite, and a
// channel served under an instance root of the case's own for the Core. The harness runs in a thread of
// the case's process.
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>

#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.hpp"
#include "core.hpp"
#include "interface_harness.hpp"

using json = nlohmann::json;

namespace {

// A channel that opens, carries two events on the standing subscription, and refuses as the cases need.
class Panel : public iv1::Interface::Service {
  public:
    grpc::Status Attach(grpc::ServerContext *,
                        grpc::ServerReaderWriter<iv1::CoreFrame, iv1::ClientFrame> *stream) override {
        iv1::CoreFrame open;
        auto *o = open.mutable_opening();
        o->set_version(1);
        o->set_subscription("standing");
        o->mutable_picture()->set_at(4);
        o->mutable_picture()->add_records()->mutable_channel()->mutable_declared()->set_name("panel");
        stream->Write(open);
        for (std::uint64_t n : {5, 6}) {
            iv1::CoreFrame f;
            f.set_call("standing");
            f.mutable_event()->set_seq(n);
            f.mutable_event()->set_type("channel.attached");
            f.mutable_event()->mutable_subject()->set_kind("channel");
            f.mutable_event()->mutable_subject()->set_identity("panel");
            stream->Write(f);
        }
        iv1::ClientFrame f;
        while (stream->Read(&f)) {
            iv1::CoreFrame out;
            out.set_call(f.call());
            auto *r = out.mutable_refusal();
            if (f.request().has_command()) {
                r->set_code("channel.suspended");
                r->set_message("the channel is suspended");
                r->mutable_suspension()->set_grade("read-only");
                r->mutable_suspension()->set_by("bench");
            } else {
                r->set_code("subject.unknown");
                r->set_message("no unit nobody is declared");
                r->mutable_subject()->set_kind("unit");
                r->mutable_subject()->set_identity("nobody");
            }
            stream->Write(out);
        }
        return grpc::Status::OK;
    }
};

// The suite's side: the channel, the socket it listens on, the harness's connection.
struct Suite {
    Panel panel;
    std::unique_ptr<grpc::Server> server;
    std::string root, control;
    int listening = -1, conn = -1;
    std::string pending, last;
    std::thread harness;
    std::atomic<bool> running{false};
    std::atomic<int> status{-1};
    int next = 0;

    Suite() {
        root = check::dir();
        std::filesystem::create_directories(root + "/interfaces");
        grpc::ServerBuilder b;
        b.AddListeningPort("unix:" + root + "/interfaces/panel.sock", grpc::InsecureServerCredentials());
        b.RegisterService(&panel);
        server = b.BuildAndStart();
        REQUIRE(server != nullptr, "the channel could not be served");
        control = root + "/control.sock";
        listening = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strcpy(addr.sun_path, control.c_str());
        REQUIRE(bind(listening, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 &&
                    listen(listening, 1) == 0,
                "the suite's socket cannot be bound");
        running = true;
        std::string c = control, r = root;
        harness = std::thread([this, c, r] {
            status = interface_harness::serve([c, r](const char *name) -> const char * {
                if (std::strcmp(name, "CONFORMANCE_SOCKET") == 0) {
                    return c.c_str();
                }
                if (std::strcmp(name, "CONFORMANCE_INSTANCE") == 0) {
                    return r.c_str();
                }
                return nullptr;
            });
            running = false;
        });
        pollfd pf{listening, POLLIN, 0};
        if (poll(&pf, 1, 5000) != 1) {
            harness.detach();
            REQUIRE(false, "the harness never connected");
        }
        conn = accept(listening, nullptr, nullptr);
    }

    ~Suite() {
        if (harness.joinable()) {
            if (running) {
                write_line(R"({"type":"finish"})");
            }
            harness.join();
        }
        close(conn);
        close(listening);
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
    }

    void write_line(const std::string &line) {
        std::string s = line + "\n";
        [[maybe_unused]] ssize_t n = write(conn, s.data(), s.size());
    }

    // The next line the harness wrote, parsed; null when none came within timeout_ms.
    json read(int timeout_ms = 5000) {
        auto until = check::ms() + timeout_ms;
        for (;;) {
            auto nl = pending.find('\n');
            if (nl != std::string::npos) {
                last = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                return json::parse(last, nullptr, false);
            }
            long long left = until - check::ms();
            pollfd pf{conn, POLLIN, 0};
            if (left <= 0 || poll(&pf, 1, static_cast<int>(left)) != 1) {
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

    // Issues a directive, and returns its result; the observations read before it go to before.
    json directive(const std::string &verb, const json &args, std::vector<json> *before = nullptr) {
        write_line(json{{"type", "directive"}, {"id", ++next}, {"verb", verb}, {"args", args}}.dump());
        for (;;) {
            json m = read();
            REQUIRE(!m.is_null(), "no result came for ", verb);
            if (m.value("type", "") == "result") {
                return m;
            }
            if (before) {
                before->push_back(m);
            }
        }
    }

    bool exited(int timeout_ms) {
        auto until = check::ms() + timeout_ms;
        while (running && check::ms() < until) {
            check::sleep(10);
        }
        return !running;
    }
};

// std: yoke-sdk-cpp:the-interface-harness.01
void test_hello_first_for_the_interface_contract() {
    Suite s;
    json hello = s.read();
    REQUIRE(hello.value("type", "") == "hello" && hello.value("contract", "") == "interface" &&
                hello.value("language", "") == "cpp" && hello.value("sdk", "") == yoke::sdk_line &&
                hello.value("version", 0) == 1 && !hello.contains("unit"),
            "the harness said ", hello.dump(), " first");
}

// std: yoke-sdk-cpp:the-interface-harness.02
void test_attaching_reports_the_opening_and_the_standing_subscription_is_observed() {
    Suite s;
    s.read();
    std::vector<json> observed;
    json r = s.directive("attach", {{"channel", "panel"}}, &observed);
    const json &value = r["value"];
    REQUIRE(value.value("version", 0) == 1 && value.value("subscription", "") == "standing" &&
                value.value("at", 0) == 4 && value["records"].size() == 1,
            "the attachment was reported as ", r.dump());
    while (observed.size() < 2) {
        json m = s.read();
        REQUIRE(!m.is_null(), "only ", observed.size(), " observations came");
        observed.push_back(m);
    }
    for (std::size_t i = 0; i < 2; i++) {
        const json &o = observed[i];
        const json &f = o["fields"];
        REQUIRE(o.value("kind", "") == "event" && f.value("seq", std::size_t{0}) == 5 + i &&
                    f.value("type", "") == "channel.attached" && f.value("subject", "") == "panel",
                "observation ", i, " is ", o.dump());
    }
}

// std: yoke-sdk-cpp:the-interface-harness.03
void test_a_refusal_is_reported_with_what_it_names() {
    Suite s;
    s.read();
    s.directive("attach", {{"channel", "panel"}});
    json read = s.directive("read", {{"kind", "unit"}, {"identity", "nobody"}});
    REQUIRE(read.value("refusal", "") == "subject.unknown" &&
                read["value"]["subject"].value("kind", "") == "unit" &&
                read["value"]["subject"].value("identity", "") == "nobody",
            "the read was reported as ", read.dump());
    json command = s.directive("command", {{"unit", "acquire"}, {"type", "calibrate"}});
    REQUIRE(command.value("refusal", "") == "channel.suspended" &&
                command["value"]["suspension"].value("grade", "") == "read-only" &&
                command["value"]["suspension"].value("by", "") == "bench",
            "the command was reported as ", command.dump());
    REQUIRE(s.last.find("suspended in") == std::string::npos && s.last.find("declared") == std::string::npos,
            "a refusal carries the library's words: ", s.last);
}

// std: yoke-sdk-cpp:the-interface-harness.04
void test_it_knows_no_other_verb_finishes_and_holds_no_wire() {
    Suite s;
    s.read();
    json r = s.directive("emit", json::object());
    REQUIRE(r.value("unrecognised", false), "emit was reported as ", r.dump());
    s.write_line(R"({"type":"finish"})");
    REQUIRE(s.exited(3000) && s.status == 0, "finish did not end the harness with 0");
    for (const char *file : {"/interface-harness/interface_harness.cpp",
                             "/interface-harness/interface_harness.hpp", "/interface-harness/main.cpp"}) {
        std::string code = check::read(std::string(SOURCE_DIR) + file);
        REQUIRE(!code.empty(), file, " cannot be read");
        for (const char *wire : {"grpcpp/", ".grpc.pb.h"}) {
            REQUIRE(code.find(wire) == std::string::npos, file, " includes ", wire);
        }
    }
}

// std: yoke-sdk-cpp:the-interface-harness.05
void test_the_suite_is_run_against_the_interface_harness() {
    std::string script = check::read(SOURCE_DIR "/ci/conformance.sh");
    REQUIRE(script.find("--target yoke-cpp-plugin-harness yoke-cpp-interface-harness") != std::string::npos,
            "ci/conformance.sh does not build the interface harness");
    REQUIRE(script.find("--harness \"$root/build/yoke-cpp-interface-harness\"") != std::string::npos,
            "ci/conformance.sh does not run the suite against the interface harness");
    REQUIRE(script.find("--harness \"$root/build/yoke-cpp-plugin-harness\"") != std::string::npos,
            "ci/conformance.sh no longer runs the suite against the plugin harness");
}

} // namespace

int main() {
    return check::run({
        {"yoke-sdk-cpp:the-interface-harness.01", test_hello_first_for_the_interface_contract},
        {"yoke-sdk-cpp:the-interface-harness.02",
         test_attaching_reports_the_opening_and_the_standing_subscription_is_observed},
        {"yoke-sdk-cpp:the-interface-harness.03", test_a_refusal_is_reported_with_what_it_names},
        {"yoke-sdk-cpp:the-interface-harness.04", test_it_knows_no_other_verb_finishes_and_holds_no_wire},
        {"yoke-sdk-cpp:the-interface-harness.05", test_the_suite_is_run_against_the_interface_harness},
    });
}
