// The library a Plugin unit is written with.
//
// One declaration is both the Manifest the library generates and the surface its registration claims,
// so the two cannot be written apart. Starting a unit performs the first acts in their order: read the
// environment, bind the unit's own socket, register, open the Session — and then beats on the terms the
// Core assigned, repeating the author's last health report. Everything the Session brings is handed to
// the author as an event, its end included: a Session that ends ends the incarnation, and the library
// never reconnects, never retries an admission, never polls, never creates a stream's transport and
// never chooses a severity or a grade.
//
// The library runs two threads of its own, one reading the Session and one beating. Every member of a
// Unit may be called from any thread.
#ifndef YOKE_PLUGIN_HPP
#define YOKE_PLUGIN_HPP

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "yoke/base.hpp"

namespace yoke::plugin {

// A stream the Plugin may publish, and what its data tolerates.
struct Stream {
    std::string id;
    bool tolerates_loss = false;
    bool tolerates_reorder = false;
};

// The kind of the one thing a capability governs.
enum class Governs { stream, command, query, occurrence, surface };

// The one thing a capability governs.
struct Object {
    Governs kind;
    std::string id;
};

// What an operator may grant, governing exactly one object.
struct Capability {
    std::string name;
    Object governs;
};

// What a Plugin says about itself: what is true of the binary wherever it runs.
struct Declaration {
    std::string id;
    std::vector<std::string> needs; // `<class>` or `<class>:<name>`
    std::vector<Stream> streams;
    std::vector<std::string> commands;
    std::vector<std::string> queries;
    std::vector<std::string> occurrences;
    std::vector<Capability> capabilities;

    // The document the declaration generates.
    std::string manifest() const;
};

// Five lists: what was granted, or what was withheld.
struct Scope {
    std::vector<std::string> capabilities, streams, commands, queries, occurrences;
};

// What the Core answered the registration with.
struct Admission {
    bool restricted = false;
    Scope granted;
    Scope withheld; // item by item
};

// An instruction the Core sent, to be acknowledged.
struct Command {
    std::string id; // the message identity an acknowledgement is correlated to
    std::string type;
    std::string payload;
};

// A query the Core asked, to be answered.
struct Question {
    std::string id; // the message identity an answer is correlated to
    std::string type;
    std::string payload;
};

// A stream may now be emitted on, and where.
struct Activated {
    std::string stream;
    std::string transport; // `ordered` or `framed`
    std::string address;
};

// A stream may no longer be emitted on.
struct Stopped {
    std::string stream;
};

// An error the Core answered a message with.
struct Refused {
    std::string correlation; // the identity of the message the Core refused
    std::string code;
    std::string message;
    std::string item;
};

// The end of the Session: closed by this unit, or revoked by the Core. Nothing follows it.
struct Ended {
    bool closed = false;
    std::string cause; // for a revocation: liveness lost, plugin disabled, scope exceeded, protocol failure
    std::string line;
};

// What the Session brings, in the order it brought it.
using Event = std::variant<Command, Question, Activated, Stopped, Refused, Ended>;

// What became of a command.
enum class Outcome { accepted = 1, done, failed };

// How routine or alarming an occurrence is, from 0 to 99: the author's statement and nobody else's.
// A default-constructed severity states nothing, and an occurrence reported with it is refused.
class Severity {
  public:
    Severity() = default;
    // A severity the author states.
    static Severity of(unsigned value) {
        Severity s;
        s.value_ = value;
        s.set_ = true;
        return s;
    }
    bool stated() const { return set_; }
    unsigned value() const { return value_; }

  private:
    unsigned value_ = 0;
    bool set_ = false;
};

// A started unit and its Session.
class Unit {
  public:
    // Starts a unit from the process's environment.
    static std::unique_ptr<Unit> start(const Declaration &declaration);

    // Starts a unit from the environment getenv reads: bind, register, open the Session. A refusal is
    // thrown with its stage and code, and nothing is tried again.
    static std::unique_ptr<Unit> start(const Declaration &declaration, const Getenv &getenv);

    // Releases the unit, ending its Session first if it has not ended.
    ~Unit();
    Unit(const Unit &) = delete;
    Unit &operator=(const Unit &) = delete;

    // What the Core answered the registration with.
    const Admission &admission() const;

    // The next event, waiting for one; nothing once the end has been handed over.
    std::optional<Event> next();

    // As next(), waiting at most wait: nothing when it passed with no event, or after the end.
    std::optional<Event> next(std::chrono::milliseconds wait);

    // Whether the Session has ended. The incarnation is over: the process should finish.
    bool done() const;

    // Ends the Session in order: a CLOSE, the unit's own departure.
    void close();

    // Says what became of a command, correlated to it.
    void ack(const Command &command, Outcome outcome, std::string_view line = {});

    // Answers a question, correlated to it.
    void answer(const Question &question, std::string_view payload);

    // Says what went wrong with a message the Core sent, correlated to it: a code and a message.
    void fail(std::string_view about, std::string_view code, std::string_view message);

    // Reports an occurrence of a declared class, with the author's severity.
    void report(std::string_view occurrence, Severity severity, std::string_view line,
                std::string_view detail = {});

    // Reports how well the unit is: a grade from 0 to 99, and a line. The library repeats the last
    // report at every beat, and sends no beat before the first: a unit keeps its liveness only once its
    // author has reported, so the first report must come within the tolerance the Core assigned.
    void health(unsigned grade, std::string_view line);

    // Sends data on a stream, on the transport its activation named: one data envelope per packet on
    // the ordered transport, one frame per datagram on the framed one, numbered from 1 within the
    // activation. Only the Core creates a stream's transport, so a stream it has not activated has
    // nowhere to be written, and the library refuses rather than make one.
    void emit(std::string_view stream, std::string_view payload);

    struct State;

  private:
    explicit Unit(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace yoke::plugin

#endif
