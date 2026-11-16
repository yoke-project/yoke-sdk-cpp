// The binding's cases. Each loads a QML document into an engine of its own, against a Core the case
// stands in for, and reads what the document saw. The module is registered into the program rather than
// loaded as a plugin, so the interface library is linked once.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>
#include <QVariant>

#include "binding.hpp"
#include "check.hpp"
#include "core.hpp"

using namespace std::chrono_literals;

namespace {

// A Core served on a local socket in the case's directory, and its path.
struct Served {
    std::string path;
    std::unique_ptr<grpc::Server> server;

    explicit Served(Core &core) : path(check::dir() + "/panel.sock") {
        server = serve(core, path);
        REQUIRE(server != nullptr, "the Core could not be served at ", path);
    }
    ~Served() { server->Shutdown(std::chrono::system_clock::now() + 1s); }
};

// The application and the engine a case's document runs in.
struct Engine {
    int argc = 1;
    char name[9] = "qml_test";
    char *argv[1] = {name};
    QCoreApplication app{argc, argv};
    QQmlEngine engine;

    Engine() {
        qmlRegisterTypesAndRevisions<yoke::qml::Attachment, yoke::qml::Subscription, yoke::qml::Delivery>(
            "Yoke.Interface", 0);
    }

    // The document, made; the case fails, saying why, where it cannot be.
    std::unique_ptr<QObject> load(const std::string &body) {
        QQmlComponent c(&engine);
        c.setData(QByteArray::fromStdString("import QtQml\nimport Yoke.Interface\n" + body),
                  QUrl::fromLocalFile(QString::fromStdString(check::dir() + "/case.qml")));
        std::unique_ptr<QObject> made(c.create());
        REQUIRE(made != nullptr, "the document did not load: ", c.errorString().toStdString());
        return made;
    }
};

// Runs the document's thread until cond holds, at most ms milliseconds; whether it came to hold.
bool until(const std::function<bool()> &cond, int ms = 3000) {
    auto deadline = check::ms() + ms;
    while (!cond()) {
        if (check::ms() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        check::sleep(1);
    }
    return true;
}

QVariant get(const std::unique_ptr<QObject> &o, const char *name) { return o->property(name); }

QVariant at(const QVariant &v, const QString &path) {
    QVariant here = v;
    for (const auto &part : path.split('.')) {
        bool number = false;
        int i = part.toInt(&number);
        here = number ? here.toList().value(i) : here.toMap().value(part);
    }
    return here;
}

std::string str(const QVariant &v) { return v.toString().toStdString(); }

std::string quoted(const std::string &s) { return "\"" + s + "\""; }

// Attached is 2 in the order of the binding's statuses: Detached, Attaching, Attached, Ended.
constexpr int attached = 2, ended = 3;

int count(Core &c, iv1::Request::OperationCase op) {
    int n = 0;
    for (const auto &r : c.requests()) {
        n += r.operation_case() == op ? 1 : 0;
    }
    return n;
}

// std: yoke-sdk-cpp:the-qml-binding.01
void test_attaching_surfaces_the_opening_as_properties() {
    Core c;
    c.opening = opening_at(1);
    Served s(c);
    Engine e;
    auto doc = e.load("Attachment { address: " + quoted(s.path) +
                      "; property var seen: []; onStatusChanged: seen = seen.concat([status]) }");
    REQUIRE(until([&] { return get(doc, "status").toInt() == attached; }), "the document never attached");
    auto seen = get(doc, "seen").toList();
    REQUIRE(seen.size() == 2 && seen[0].toInt() == 1 && seen[1].toInt() == attached,
            "the status passed through ", str(QVariant(seen).toStringList().join(',')));
    REQUIRE(get(doc, "version").toInt() == 1, "version is ", get(doc, "version").toInt());
    REQUIRE(str(get(doc, "subscription")) == "standing", "subscription is ", str(get(doc, "subscription")));
    REQUIRE(at(get(doc, "picture"), "at").toULongLong() == 4, "picture.at is ",
            str(at(get(doc, "picture"), "at")));
    auto records = get(doc, "records").toList();
    REQUIRE(records.size() == 1 && str(at(records, "0.unit.declared.identity")) == "acquire", "records hold ",
            records.size(), ", the first named ", str(at(records, "0.unit.declared.identity")));
}

// std: yoke-sdk-cpp:the-qml-binding.02
void test_every_operation_returns_at_once_and_answers_later() {
    Core c;
    c.opening = opening_at(1);
    c.answer = [](const std::string &call, const iv1::Request &r, const Send &send) {
        iv1::Response resp;
        if (r.has_query()) {
            check::sleep(300);
            resp.mutable_query()->set_payload(r.query().unit());
        } else if (r.has_command()) {
            resp.mutable_command()->set_outcome(iv1::Acknowledged::OUTCOME_DONE);
            resp.mutable_command()->set_line("ok");
        } else {
            resp.mutable_read()->add_records()->mutable_unit()->mutable_declared()->set_identity("acquire");
        }
        send(answered(call, resp));
    };
    Served s(c);
    Engine e;
    auto doc =
        e.load("Attachment {\n"
               "  address: " +
               quoted(s.path) +
               "\n"
               "  property var log: []\n"
               "  property int took: -1\n"
               "  property var numbers: []\n"
               "  property var answeredCalls: []\n"
               "  property string refusedCode: ''\n"
               "  property var refusedCalls: []\n"
               "  onAnswered: (call, answer) => answeredCalls = answeredCalls.concat([call])\n"
               "  onRefused: (call, refusal) => refusedCalls = refusedCalls.concat([call])\n"
               "  onStatusChanged: if (attached) {\n"
               "    const t = Date.now()\n"
               "    const n1 = ask('slow', 'status', 'hello', a => log = log.concat(['ask:' + text(a)]))\n"
               "    const n2 = command('fast', 'calibrate', '', k => log = log.concat(['command:' + "
               "k.outcome + ':' + k.line]))\n"
               "    const n3 = read('unit', '', rs => log = log.concat(['read:' + rs.length]))\n"
               "    took = Date.now() - t\n"
               "    const n4 = command('fast', 'calibrate', {a: 1}, null, r => refusedCode = r.code)\n"
               "    numbers = [n1, n2, n3, n4]\n"
               "  }\n"
               "}");
    REQUIRE(
        until([&] { return get(doc, "log").toList().size() == 3 && !str(get(doc, "refusedCode")).empty(); }),
        "the answers did not all arrive: ", str(QVariant(get(doc, "log").toStringList().join(' '))));
    int took = get(doc, "took").toInt();
    REQUIRE(took >= 0 && took < 50, "the three calls took ", took, " ms to return");
    auto log = get(doc, "log").toStringList();
    REQUIRE(log.size() == 3 && log[2] == "ask:slow" && log.contains("command:OUTCOME_DONE:ok") &&
                log.contains("read:1"),
            "the answers arrived as ", log.join(' ').toStdString());
    auto numbers = get(doc, "numbers").toList();
    REQUIRE(numbers.size() == 4 && numbers[0] != numbers[1] && numbers[1] != numbers[2] &&
                numbers[2] != numbers[3],
            "the calls were not numbered each their own");
    REQUIRE(get(doc, "answeredCalls").toList().contains(numbers[1]),
            "the command's answer was not signalled");
    REQUIRE(str(get(doc, "refusedCode")) == "binding.payload", "the payload of neither kind was refused '",
            str(get(doc, "refusedCode")), "'");
    REQUIRE(get(doc, "refusedCalls").toList().contains(numbers[3]), "the refusal was not signalled");
    REQUIRE(count(c, iv1::Request::kCommand) == 1, "the Core was sent ", count(c, iv1::Request::kCommand),
            " commands, and the one whose payload was refused should not have been sent");
}

// std: yoke-sdk-cpp:the-qml-binding.03
void test_a_refusal_reaches_the_document_with_its_code_and_its_detail() {
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
    Engine e;
    auto doc = e.load("Attachment {\n"
                      "  address: " +
                      quoted(s.path) +
                      "\n"
                      "  property var readRefusal\n"
                      "  property var commandRefusal\n"
                      "  property var refusedCalls: []\n"
                      "  onRefused: (call, refusal) => refusedCalls = refusedCalls.concat([call])\n"
                      "  onStatusChanged: if (attached) {\n"
                      "    read('unit', 'nobody', null, r => readRefusal = r)\n"
                      "    command('acquire', 'calibrate', '', null, r => commandRefusal = r)\n"
                      "  }\n"
                      "}");
    REQUIRE(until([&] { return get(doc, "readRefusal").isValid() && get(doc, "commandRefusal").isValid(); }),
            "the refusals did not arrive");
    auto r = get(doc, "readRefusal");
    REQUIRE(str(at(r, "code")) == "subject.unknown" && str(at(r, "subject.kind")) == "unit" &&
                str(at(r, "subject.identity")) == "nobody",
            "the read was refused ", str(at(r, "code")), " naming ", str(at(r, "subject.kind")), " ",
            str(at(r, "subject.identity")));
    auto k = get(doc, "commandRefusal");
    REQUIRE(str(at(k, "code")) == "channel.suspended" && str(at(k, "grade")) == "read-only" &&
                str(at(k, "by")) == "service",
            "the command was refused ", str(at(k, "code")), " in grade ", str(at(k, "grade")), " by ",
            str(at(k, "by")));
    REQUIRE(get(doc, "refusedCalls").toList().size() == 2, "refused was signalled ",
            get(doc, "refusedCalls").toList().size(), " times");
}

// std: yoke-sdk-cpp:the-qml-binding.04
void test_subscriptions_arrive_as_signals_and_only_the_document_confirms() {
    Core c;
    c.opening = opening_at(1);
    iv1::Response overflow;
    overflow.mutable_subscribe()->mutable_overflow()->set_at(9);
    c.standing = {event_at(5), event_at(6), event_at(7), answered("standing", overflow)};
    c.answer = [](const std::string &call, const iv1::Request &r, const Send &send) {
        iv1::Response resp;
        if (r.has_confirm()) {
            resp.mutable_confirm();
            send(answered(call, resp));
            return;
        }
        resp.mutable_subscribe()->mutable_snapshot()->set_at(20);
        send(answered(call, resp));
        for (std::uint64_t n : {21, 22}) {
            iv1::CoreFrame f;
            f.set_call(call);
            f.mutable_event()->set_seq(n);
            send(f);
        }
    };
    Served s(c);
    Engine e;
    auto doc = e.load("QtObject {\n"
                      "  property var standingSeen: []\n"
                      "  property var seen: []\n"
                      "  property Attachment a: Attachment {\n"
                      "    address: " +
                      quoted(s.path) +
                      "\n"
                      "    onReceived: (event) => standingSeen = standingSeen.concat([event.seq])\n"
                      "  }\n"
                      "  property Subscription sub: Subscription {\n"
                      "    attachment: a\n"
                      "    subjectKind: 'unit'\n"
                      "    onSnapshot: (s) => seen = seen.concat(['snapshot:' + s.at])\n"
                      "    onReceived: (event) => seen = seen.concat(['event:' + event.seq])\n"
                      "  }\n"
                      "}");
    REQUIRE(until([&] {
                return get(doc, "standingSeen").toList().size() == 3 && get(doc, "seen").toList().size() == 3;
            }),
            "the deliveries did not arrive: standing ", get(doc, "standingSeen").toList().size(),
            ", subscription ", get(doc, "seen").toList().size());
    auto standing = get(doc, "standingSeen").toList();
    REQUIRE(standing[0].toInt() == 5 && standing[1].toInt() == 6 && standing[2].toInt() == 7,
            "the standing subscription's events arrived out of order");
    QObject *a = get(doc, "a").value<QObject *>();
    REQUIRE(until([&] { return at(a->property("picture"), "at").toInt() == 9; }),
            "the overflow did not become the picture");
    auto seen = get(doc, "seen").toStringList();
    REQUIRE(seen.join(' ') == "snapshot:20 event:21 event:22", "the subscription delivered ",
            seen.join(' ').toStdString());
    QObject *sub = get(doc, "sub").value<QObject *>();
    REQUIRE(sub->property("open").toBool(), "the subscription is not open");
    std::string id = str(sub->property("id"));
    REQUIRE(!id.empty(), "the subscription has no id");
    bool kind = false;
    for (const auto &r : c.requests()) {
        kind = kind || (r.has_subscribe() && r.subscribe().filter().subject_kind() == "unit");
    }
    REQUIRE(kind, "the Core was not sent the filter's kind");

    check::sleep(200);
    QCoreApplication::processEvents();
    REQUIRE(count(c, iv1::Request::kConfirm) == 0, "a confirmation was sent with nothing confirmed");
    QMetaObject::invokeMethod(sub, "confirm", Q_ARG(double, 21.0), Q_ARG(QJSValue, QJSValue()),
                              Q_ARG(QJSValue, QJSValue()));
    REQUIRE(until([&] { return count(c, iv1::Request::kConfirm) == 1; }), "the confirmation was not sent");
    for (const auto &r : c.requests()) {
        if (r.has_confirm()) {
            REQUIRE(r.confirm().subscription() == id && r.confirm().sequence() == 21, "the confirmation was ",
                    r.ShortDebugString());
        }
    }
}

// std: yoke-sdk-cpp:the-qml-binding.05
void test_a_streams_frames_arrive_as_signals_and_letting_it_go_releases_it() {
    Core c;
    c.opening = opening_at(1);
    c.answer = [](const std::string &call, const iv1::Request &r, const Send &send) {
        iv1::Response resp;
        if (r.has_stream_unsubscribe()) {
            resp.mutable_stream_unsubscribe();
            send(answered(call, resp));
            return;
        }
        auto *d = resp.mutable_stream_subscribe();
        d->set_delivery("d-1"), d->set_flowing(true), d->set_connection(true);
        send(answered(call, resp));
        check::sleep(50);
        std::uint64_t i = 0;
        for (const char *p : {"one", "two", "three"}) {
            iv1::CoreFrame f;
            f.set_call("d-1");
            f.mutable_delivery()->set_delivery("d-1");
            f.mutable_delivery()->set_sequence(i + 1);
            f.mutable_delivery()->set_sent_at(1001 + i);
            f.mutable_delivery()->set_payload(p);
            send(f);
            i++;
        }
    };
    Served s(c);
    Engine e;
    auto doc = e.load(
        "QtObject {\n"
        "  property var seen: []\n"
        "  property Attachment a: Attachment { address: " +
        quoted(s.path) +
        " }\n"
        "  property Delivery d: Delivery {\n"
        "    attachment: a; unit: 'hw'; stream: 'cpu'\n"
        "    onFrame: (f) => seen = seen.concat([f.sequence + '@' + f.sentAt + ':' + a.text(f.payload)])\n"
        "  }\n"
        "}");
    REQUIRE(until([&] { return get(doc, "seen").toList().size() == 3; }),
            "the frames did not arrive: ", get(doc, "seen").toList().size());
    QObject *d = get(doc, "d").value<QObject *>();
    REQUIRE(str(d->property("id")) == "d-1" && d->property("flowing").toBool() &&
                str(d->property("arrives")) == "connection",
            "the delivery opened as ", str(d->property("id")), ", flowing ", d->property("flowing").toBool(),
            ", arriving on ", str(d->property("arrives")));
    auto seen = get(doc, "seen").toStringList();
    REQUIRE(seen.join(' ') == "1@1001:one 2@1002:two 3@1003:three", "the frames arrived as ",
            seen.join(' ').toStdString());
    d->setProperty("active", false);
    REQUIRE(until([&] { return count(c, iv1::Request::kStreamUnsubscribe) == 1; }),
            "making the delivery inactive did not release it");
    for (const auto &r : c.requests()) {
        if (r.has_stream_unsubscribe()) {
            REQUIRE(r.stream_unsubscribe().delivery() == "d-1", "the release named ",
                    r.stream_unsubscribe().delivery());
        }
    }
}

// std: yoke-sdk-cpp:the-qml-binding.06
void test_an_attachment_the_core_ended_is_a_state_the_document_reads() {
    Core c;
    c.opening = opening_at(1);
    c.end_after = 1;
    c.answer = [](const std::string &call, const iv1::Request &, const Send &send) {
        iv1::Response resp;
        resp.mutable_read();
        send(answered(call, resp));
    };
    Served s(c);
    Engine e;
    auto doc = e.load(
        "Attachment {\n"
        "  address: " +
        quoted(s.path) +
        "\n"
        "  property string after: ''\n"
        "  onStatusChanged: {\n"
        "    if (attached) read('unit')\n"
        "    if (status === Attachment.Ended) command('fast', 'calibrate', '', null, r => after = r.code)\n"
        "  }\n"
        "}");
    REQUIRE(until([&] { return get(doc, "status").toInt() == ended; }), "the attachment never read as ended");
    REQUIRE(!get(doc, "attached").toBool(), "an ended attachment reads as attached");
    REQUIRE(!str(at(get(doc, "failure"), "code")).empty(), "the ended attachment's failure carries no code");
    REQUIRE(until([&] { return str(get(doc, "after")) == "binding.detached"; }),
            "a command after the end was answered '", str(get(doc, "after")), "'");

    auto started = check::ms();
    auto nowhere = e.load("Attachment { address: " + quoted(check::dir() + "/nobody.sock") + " }");
    auto took = check::ms() - started;
    REQUIRE(took < 100, "loading a document attaching nowhere took ", took, " ms on its thread");
    REQUIRE(until([&] { return get(nowhere, "status").toInt() == ended; }, 35000),
            "an attachment to no Core never read as ended");
    REQUIRE(!str(at(get(nowhere, "failure"), "message")).empty(),
            "the failure to attach says nothing of why");
}

// std: yoke-sdk-cpp:the-qml-binding.07
void test_nothing_of_the_plugin_or_interface_library_links_qt() {
    std::string symbols;
    FILE *nm = popen("nm -C " YOKE_CPP_LIBRARY " 2>/dev/null", "r");
    REQUIRE(nm != nullptr, "the library's symbols cannot be listed");
    char line[4096];
    while (fgets(line, sizeof line, nm) != nullptr) {
        symbols += line;
    }
    REQUIRE(pclose(nm) == 0 && !symbols.empty(), "nm read no symbols from " YOKE_CPP_LIBRARY);
    for (const char *qt : {"QObject", "QString", "QVariant", "QMetaObject", "qt_", "Qt6"}) {
        auto at = symbols.find(qt);
        REQUIRE(at == std::string::npos, "the library names Qt's ", qt, ": ",
                symbols.substr(symbols.rfind('\n', at) + 1, 120));
    }
}

} // namespace

int main() {
    // Qt reads its text as UTF-8 and says so where the locale does not; the cases' lines are the record's.
    setenv("LC_ALL", "C.UTF-8", 1);
    return check::run({
        {"yoke-sdk-cpp:the-qml-binding.01", test_attaching_surfaces_the_opening_as_properties},
        {"yoke-sdk-cpp:the-qml-binding.02", test_every_operation_returns_at_once_and_answers_later},
        {"yoke-sdk-cpp:the-qml-binding.03", test_a_refusal_reaches_the_document_with_its_code_and_its_detail},
        {"yoke-sdk-cpp:the-qml-binding.04",
         test_subscriptions_arrive_as_signals_and_only_the_document_confirms},
        {"yoke-sdk-cpp:the-qml-binding.05",
         test_a_streams_frames_arrive_as_signals_and_letting_it_go_releases_it},
        {"yoke-sdk-cpp:the-qml-binding.06", test_an_attachment_the_core_ended_is_a_state_the_document_reads},
        {"yoke-sdk-cpp:the-qml-binding.07", test_nothing_of_the_plugin_or_interface_library_links_qt},
    });
}
