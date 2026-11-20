// The interface library: the interface contract on its typed projection, over the base.
#include "yoke/interface.hpp"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <algorithm>
#include <cstdint>
#include <google/protobuf/any.pb.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>
#include <grpcpp/create_channel_posix.h>
#include <grpcpp/grpcpp.h>
#include <linux/futex.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>

#include "tls.hpp"
#include "wire.hpp"
#include "yoke/interface/v1/contract.pb.h"
#include "yoke/interface/v1/interface.grpc.pb.h"

namespace yoke::iface {

int contract() { return v1::CONTRACT_VERSION; }

namespace {

// A name that could choose where a socket is cannot name anything.
bool names_a_place(const std::string &name) {
    return name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
           name.find('\0') != std::string::npos;
}

std::string channel_socket(const std::string &root, const std::string &channel) {
    if (names_a_place(channel)) {
        throw std::runtime_error("the channel name \"" + channel + "\" cannot name a channel");
    }
    return root + "/interfaces/" + channel + ".sock";
}

} // namespace

std::string service(const std::string &runtime_dir, const std::string &channel) {
    return channel_socket(runtime_dir, channel);
}

std::string application(const std::string &instance, const std::string &channel, const Getenv &getenv) {
    if (names_a_place(instance)) {
        throw std::runtime_error("the instance name \"" + instance + "\" cannot name an instance");
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || *runtime == '\0') {
        throw std::runtime_error(
            "XDG_RUNTIME_DIR is not set, and an application instance's addresses derive from it");
    }
    return channel_socket(std::string(runtime) + "/yoke/" + instance, channel);
}

std::string managed(const Getenv &getenv) {
    const char *socket = getenv("YOKE_SOCKET");
    if (socket == nullptr || *socket == '\0') {
        throw std::runtime_error("YOKE_SOCKET is not set: a managed interface is handed its channel's "
                                 "address, and this process was not");
    }
    return socket;
}

std::string name(const v1::Request &request) {
    switch (request.operation_case()) {
    case v1::Request::kAuthenticate:
        return "authenticate";
    case v1::Request::kRead:
        return "read";
    case v1::Request::kSubscribe:
        return "subscribe";
    case v1::Request::kConfirm:
        return "confirm";
    case v1::Request::kCommand:
        return "command";
    case v1::Request::kQuery:
        return "query";
    case v1::Request::kStreamStart:
        return "stream.start";
    case v1::Request::kStreamStop:
        return "stream.stop";
    case v1::Request::kStreamSubscribe:
        return "stream.subscribe";
    case v1::Request::kStreamUnsubscribe:
        return "stream.unsubscribe";
    case v1::Request::kReclaim:
        return "reclaim";
    default:
        return "";
    }
}

Options Options::tls(std::string roots, std::string server_name) {
    Options o;
    o.use_tls = true;
    o.roots = std::move(roots);
    o.server_name = std::move(server_name);
    return o;
}

Options &Options::password(std::string account, std::string secret) {
    v1::Authenticate a;
    a.mutable_password()->set_account(std::move(account));
    a.mutable_password()->set_secret(std::move(secret));
    credential = std::move(a);
    return *this;
}

Options &Options::token(std::string token) {
    v1::Authenticate a;
    a.set_token(std::move(token));
    credential = std::move(a);
    return *this;
}

namespace {

// A refusal as the base's error, with its detail; a suspension's as Suspended.
[[noreturn]] void throw_refusal(const v1::Refusal &r) {
    Subject subject;
    if (r.has_subject()) {
        subject = Subject{r.subject().kind(), r.subject().identity(), r.subject().incarnation()};
    }
    Refusal out(r.code(), r.message(), {}, r.item(), subject);
    if (r.has_suspension()) {
        throw Suspended(out, r.suspension().grade(), r.suspension().by());
    }
    throw out;
}

// The refusal a transport's status carries in its details — a google.rpc.Status, whose third field holds
// Anys — or nothing where it carries none.
std::optional<v1::Refusal> refusal_in(const grpc::Status &status) {
    const std::string &details = status.error_details();
    google::protobuf::io::CodedInputStream in(reinterpret_cast<const std::uint8_t *>(details.data()),
                                              static_cast<int>(details.size()));
    using Wire = google::protobuf::internal::WireFormatLite;
    while (std::uint32_t tag = in.ReadTag()) {
        if (Wire::GetTagFieldNumber(tag) == 3 &&
            Wire::GetTagWireType(tag) == Wire::WIRETYPE_LENGTH_DELIMITED) {
            std::string bytes;
            std::uint32_t length = 0;
            if (!in.ReadVarint32(&length) || !in.ReadString(&bytes, static_cast<int>(length))) {
                return std::nullopt;
            }
            google::protobuf::Any any;
            v1::Refusal r;
            if (any.ParseFromString(bytes) && any.UnpackTo(&r)) {
                return r;
            }
        } else if (!Wire::SkipField(&in, tag)) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[noreturn]] void throw_status(const grpc::Status &status) {
    if (auto r = refusal_in(status)) {
        throw_refusal(*r);
    }
    throw std::runtime_error("the attachment failed: " + status.error_message());
}

// The frames of one call in the order they arrived, however many, so a call nobody is reading never holds
// up the others.
class Queue {
  public:
    void push(v1::CoreFrame f) {
        {
            std::lock_guard<std::mutex> hold(mu_);
            frames_.push_back(std::move(f));
        }
        ready_.notify_all();
    }

    void close() {
        {
            std::lock_guard<std::mutex> hold(mu_);
            closed_ = true;
        }
        ready_.notify_all();
    }

    enum class Popped { frame, closed, timed_out };

    // The next frame into f, waiting until the deadline; or that the queue closed empty.
    Popped pop(v1::CoreFrame &f, std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> hold(mu_);
        if (!ready_.wait_until(hold, deadline, [this] { return !frames_.empty() || closed_; })) {
            return Popped::timed_out;
        }
        if (frames_.empty()) {
            return Popped::closed;
        }
        f = std::move(frames_.front());
        frames_.pop_front();
        return Popped::frame;
    }

    Popped pop(v1::CoreFrame &f) { return pop(f, std::chrono::steady_clock::time_point::max()); }

  private:
    std::mutex mu_;
    std::condition_variable ready_;
    std::deque<v1::CoreFrame> frames_;
    bool closed_ = false;
};

// The call identity of the credential an attachment presents first.
const char *const authenticating = "c-1";

} // namespace

struct Attachment::State {
    std::unique_ptr<tls::Pump> pump; // on a routable address; let go after the channel
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<v1::Interface::Stub> stub;
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReaderWriter<v1::ClientFrame, v1::CoreFrame>> stream;
    std::thread reader;

    std::mutex sending;
    std::mutex mu;
    std::map<std::string, std::shared_ptr<Queue>> calls;
    bool ended = false;
    bool closed = false;
    int count = 0;
    std::condition_variable received_cv;
    bool received = false; // nothing more arrives from the Core
    grpc::Status status;   // how the attachment ended, once it has

    // The opening, and what arrived before it: the reader hands the first frame here.
    std::shared_ptr<Queue> opening = std::make_shared<Queue>();

    // The deliveries read from a socket of their own, which end with the attachment.
    std::vector<std::weak_ptr<Delivery::State>> deliveries;

    v1::Snapshot picture;
    int version = 0;
    std::optional<v1::Authenticated> established;
    std::unique_ptr<Subscription> standing;

    ~State() {
        if (reader.joinable()) {
            reader.join();
        }
    }

    std::shared_ptr<Queue> enroll(const std::string &call) {
        auto q = std::make_shared<Queue>();
        std::lock_guard<std::mutex> hold(mu);
        if (ended) {
            q->close();
            return q;
        }
        auto [it, added] = calls.emplace(call, q);
        return it->second;
    }

    void forget(const std::string &call) {
        std::lock_guard<std::mutex> hold(mu);
        calls.erase(call);
    }

    bool send(const v1::ClientFrame &f) {
        std::lock_guard<std::mutex> hold(sending);
        return !closed && stream->Write(f);
    }

    // Hands every frame to the call it belongs to, until the attachment ends. The first is the opening;
    // the standing subscription it names is enrolled before anything else is read, and a delivery on the
    // connection as its answer arrives, so none of their frames can arrive before they are.
    void receive() {
        v1::CoreFrame f;
        bool first = true;
        while (stream->Read(&f)) {
            if (first) {
                first = false;
                if (f.has_opening() && !f.opening().subscription().empty()) {
                    enroll(f.opening().subscription());
                }
                opening->push(std::move(f));
                continue;
            }
            if (f.has_answer() && f.answer().has_stream_subscribe() &&
                f.answer().stream_subscribe().connection()) {
                enroll(f.answer().stream_subscribe().delivery());
            }
            std::shared_ptr<Queue> q;
            {
                std::lock_guard<std::mutex> hold(mu);
                auto it = calls.find(f.call());
                if (it != calls.end()) {
                    q = it->second;
                }
            }
            if (q) {
                q->push(std::move(f));
            }
        }
        // How it ended is known before anyone waiting is told it ended.
        {
            std::lock_guard<std::mutex> hold(sending);
            status = stream->Finish();
        }
        {
            std::lock_guard<std::mutex> hold(mu);
            ended = true;
            for (auto &[call, q] : calls) {
                q->close();
            }
            calls.clear();
            received = true;
        }
        opening->close();
        received_cv.notify_all();
    }

    // Sends a request under a new call identity, stating the contract's version.
    std::pair<std::string, std::shared_ptr<Queue>> issue(v1::Request r) {
        r.set_version(static_cast<std::uint32_t>(contract()));
        std::string call;
        {
            std::lock_guard<std::mutex> hold(mu);
            if (ended) {
                throw Ended();
            }
            call = "c-" + std::to_string(++count);
        }
        auto q = enroll(call);
        v1::ClientFrame f;
        f.set_call(call);
        *f.mutable_request() = std::move(r);
        if (!send(f)) {
            forget(call);
            throw Ended();
        }
        return {call, q};
    }

    void cancel(const std::string &call) {
        v1::ClientFrame f;
        f.set_call(call);
        f.mutable_cancel();
        send(f);
    }

    // A call's first frame as its answer: a refusal is thrown as the base's error, an attachment that
    // ended as Ended, and a call that waited too long is cancelled on the wire and said so.
    v1::Response first(const std::string &call, Queue &q, std::chrono::milliseconds wait) {
        v1::CoreFrame f;
        switch (q.pop(f, std::chrono::steady_clock::now() + wait)) {
        case Queue::Popped::timed_out:
            cancel(call);
            throw std::runtime_error("the call " + call + " was not answered within " +
                                     std::to_string(wait.count()) + " ms");
        case Queue::Popped::closed:
            throw Ended();
        case Queue::Popped::frame:
            break;
        }
        if (f.has_refusal()) {
            throw_refusal(f.refusal());
        }
        return f.answer();
    }

    // A request answered once.
    v1::Response call(v1::Request r, std::chrono::milliseconds wait) {
        auto [id, q] = issue(std::move(r));
        try {
            v1::Response out = first(id, *q, wait);
            forget(id);
            return out;
        } catch (...) {
            forget(id);
            throw;
        }
    }

    // Ends what is still read from a delivery's socket.
    void end_deliveries();

    // Ends the attachment in order, once.
    void close() {
        {
            std::lock_guard<std::mutex> hold(sending);
            if (closed) {
                return;
            }
            closed = true;
            stream->WritesDone();
        }
        {
            std::unique_lock<std::mutex> hold(mu);
            if (!received_cv.wait_for(hold, std::chrono::seconds(1), [this] { return received; })) {
                hold.unlock();
                context.TryCancel();
            }
        }
        if (reader.joinable()) {
            reader.join();
        }
        end_deliveries();
        stub.reset();
        channel.reset();
        pump.reset();
    }
};

struct Subscription::State {
    std::shared_ptr<Attachment::State> a; // none for the standing subscription
    std::string id;
    std::shared_ptr<Queue> frames;
    std::optional<v1::Subscribed> pending;
    bool standing = false;
    std::once_flag closed;
};

Subscription::Subscription(std::shared_ptr<State> state) : state_(std::move(state)) {}

Subscription::~Subscription() { close(); }

const std::string &Subscription::id() const { return state_->id; }

std::optional<v1::Subscribed> Subscription::next() {
    if (state_->pending) {
        auto p = std::move(state_->pending);
        state_->pending.reset();
        return p;
    }
    v1::CoreFrame f;
    if (state_->frames->pop(f) != Queue::Popped::frame) {
        throw Ended();
    }
    if (f.has_refusal()) {
        throw_refusal(f.refusal());
    }
    if (f.has_completion()) {
        return std::nullopt;
    }
    if (f.has_event()) {
        v1::Subscribed s;
        *s.mutable_event() = f.event();
        return s;
    }
    return f.answer().subscribe();
}

void Subscription::close() {
    std::call_once(state_->closed, [this] {
        if (state_->standing) {
            return;
        }
        state_->a->cancel(state_->id);
        state_->a->forget(state_->id);
        state_->frames->close();
    });
}

namespace {

// The header of a frame on a per-subscriber socket: the sequence and the clock, little-endian.
constexpr std::size_t header = 16;
// The largest frame a per-subscriber socket carries: a payload of 1 MiB and its header.
constexpr std::size_t packet = header + (1 << 20);

std::uint64_t little(const char *p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    }
    return v;
}

// A u32, little-endian.
std::size_t little32(const char *p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    }
    return v;
}

} // namespace

struct Delivery::State {
    std::shared_ptr<Attachment::State> a;
    std::string id;
    bool flowing = false;
    std::string arrives = "socket";
    int socket = -1;
    std::shared_ptr<Queue> frames;
    std::string buf;
    // A stream socket: each frame after its length.
    bool lengthed = false;
    // On the shared ring: the socket is the lifeline, and the ring of the activation being read is mapped.
    bool shared = false;
    unsigned char *ring = nullptr;
    std::size_t ring_size = 0;
    std::uint32_t slots = 0, capacity = 0;
    std::uint64_t next = 1;
    // The ring's word moved with nothing published: the Core does so after every stop and every end, and
    // the lifeline's message is then on its way, so the reader waits for it there.
    bool woken = false;

    void unmap() {
        if (ring != nullptr) {
            ::munmap(ring, ring_size);
            ring = nullptr;
        }
    }
    ~State() {
        if (socket >= 0) {
            ::close(socket);
        }
        unmap();
    }
};

namespace {

// Reads exactly n bytes from a stream socket; false at its end.
bool read_exactly(int fd, char *buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(r);
    }
    return true;
}

// The descriptor a message carried, or -1.
int handed(msghdr &m) {
    int fd = -1;
    for (cmsghdr *c = CMSG_FIRSTHDR(&m); c != nullptr; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            std::memcpy(&fd, CMSG_DATA(c), sizeof fd);
        }
    }
    return fd;
}

// Where the ring's header keeps the word every published frame increments and wakes.
constexpr std::size_t wake_offset = 32;

// Maps a ring the lifeline handed, for reading alone.
void map_ring(Delivery::State &st, int fd) {
    struct stat info {};
    if (::fstat(fd, &info) != 0 || info.st_size < 64) {
        throw std::runtime_error("the ring handed cannot be read");
    }
    auto *mem = static_cast<unsigned char *>(
        ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0));
    if (mem == MAP_FAILED) {
        throw std::runtime_error(std::string("the ring handed cannot be mapped: ") + std::strerror(errno));
    }
    std::uint32_t version, slots, capacity;
    std::memcpy(&version, mem + 8, 4);
    std::memcpy(&slots, mem + 12, 4);
    std::memcpy(&capacity, mem + 16, 4);
    if (std::memcmp(mem, "YOKERING", 8) != 0 || version != 1 || slots == 0 ||
        64 + static_cast<std::uint64_t>(slots) * (32 + static_cast<std::uint64_t>(capacity)) >
            static_cast<std::uint64_t>(info.st_size)) {
        ::munmap(mem, static_cast<std::size_t>(info.st_size));
        throw std::runtime_error("the memory handed is not a ring this library reads");
    }
    st.unmap();
    st.ring = mem;
    st.ring_size = static_cast<std::size_t>(info.st_size);
    st.slots = slots;
    st.capacity = capacity;
    st.next = 1;
}

// One frame of the mapped ring, if one is published: a lap by the producer, or a slot overwritten while it
// was copied, is a gap the sequence shows.
std::optional<Frame> read_ring(Delivery::State &st) {
    for (;;) {
        auto published = __atomic_load_n(reinterpret_cast<std::uint64_t *>(st.ring + 24), __ATOMIC_ACQUIRE);
        if (published < st.next) {
            return std::nullopt;
        }
        if (published >= st.slots && st.next < published - st.slots + 1) {
            st.next = published - st.slots + 1;
        }
        std::uint64_t n = st.next++;
        unsigned char *slot =
            st.ring + 64 + ((n - 1) % st.slots) * (32 + static_cast<std::size_t>(st.capacity));
        auto before = __atomic_load_n(reinterpret_cast<std::uint64_t *>(slot), __ATOMIC_ACQUIRE);
        std::uint64_t clock;
        std::uint32_t length;
        std::memcpy(&clock, slot + 8, 8);
        std::memcpy(&length, slot + 16, 4);
        length = std::min(length, st.capacity);
        std::string payload(reinterpret_cast<const char *>(slot + 32), length);
        auto after = __atomic_load_n(reinterpret_cast<std::uint64_t *>(slot), __ATOMIC_ACQUIRE);
        if (before != n || after != n) {
            continue;
        }
        return Frame{n, clock, std::move(payload)};
    }
}

} // namespace

void Attachment::State::end_deliveries() {
    std::lock_guard<std::mutex> hold(mu);
    for (auto &held : deliveries) {
        if (auto d = held.lock(); d && d->socket >= 0) {
            ::shutdown(d->socket, SHUT_RDWR);
        }
    }
    deliveries.clear();
}

Delivery::Delivery(std::shared_ptr<State> state) : state_(std::move(state)) {}

Delivery::~Delivery() = default;

const std::string &Delivery::id() const { return state_->id; }

bool Delivery::flowing() const { return state_->flowing; }

const std::string &Delivery::arrives() const { return state_->arrives; }

std::optional<Frame> Delivery::next() {
    if (state_->shared) {
        // The lifeline's messages as they come — a ring on one byte, a stop of length zero, the end at its
        // closing. With no ring mapped the lifeline is all there is to wait on; with one, the wait is on the
        // ring's wake word with no bound: the producer wakes it at every frame, and the Core after every
        // stop and every end the lifeline carries, so when the word moves with nothing published the lifeline
        // is waited on, its message on its way, and never read at an interval.
        for (;;) {
            pollfd p{state_->socket, POLLIN, 0};
            int ready = ::poll(&p, 1, state_->ring != nullptr && !state_->woken ? 0 : -1);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready > 0) {
                state_->woken = false;
                unsigned char head[4];
                char space[CMSG_SPACE(sizeof(int))];
                iovec io{head, 4};
                msghdr m{};
                m.msg_iov = &io;
                m.msg_iovlen = 1;
                m.msg_control = space;
                m.msg_controllen = sizeof space;
                if (::recvmsg(state_->socket, &m, MSG_WAITALL) <= 0) {
                    state_->unmap();
                    return std::nullopt;
                }
                int fd = handed(m);
                std::uint32_t length =
                    head[0] | head[1] << 8 | head[2] << 16 | static_cast<std::uint32_t>(head[3]) << 24;
                if (length == 0) {
                    state_->unmap();
                    continue;
                }
                char body[16];
                iovec bio{body, std::min<std::size_t>(length, sizeof body)};
                msghdr bm{};
                bm.msg_iov = &bio;
                bm.msg_iovlen = 1;
                bm.msg_control = space;
                bm.msg_controllen = sizeof space;
                if (::recvmsg(state_->socket, &bm, MSG_WAITALL) <= 0) {
                    state_->unmap();
                    return std::nullopt;
                }
                if (fd < 0) {
                    fd = handed(bm);
                }
                if (fd >= 0) {
                    try {
                        map_ring(*state_, fd);
                    } catch (...) {
                        ::close(fd);
                        throw;
                    }
                    ::close(fd);
                }
                continue;
            }
            if (state_->ring != nullptr) {
                auto *wake = reinterpret_cast<std::uint32_t *>(state_->ring + wake_offset);
                auto seen = __atomic_load_n(wake, __ATOMIC_ACQUIRE);
                if (auto f = read_ring(*state_)) {
                    state_->woken = false;
                    return f;
                }
                // Shared, not private: the producer and the Core that wake it are other processes.
                bool woke = ::syscall(SYS_futex, wake, FUTEX_WAIT, seen, nullptr, nullptr, 0) == 0;
                if (auto f = read_ring(*state_)) {
                    return f;
                }
                // Woken with nothing published: the wake was the Core's.
                state_->woken = woke;
            }
        }
    }
    if (state_->socket >= 0 && state_->lengthed) {
        char head[4];
        if (!read_exactly(state_->socket, head, 4)) {
            return std::nullopt;
        }
        std::string body(little32(head), '\0');
        if (!read_exactly(state_->socket, body.data(), body.size())) {
            return std::nullopt;
        }
        if (body.size() < header) {
            throw std::runtime_error("a frame of " + std::to_string(body.size()) +
                                     " bytes is shorter than its header");
        }
        return Frame{little(body.data()), little(body.data() + 8), body.substr(header)};
    }
    if (state_->socket >= 0) {
        ssize_t n;
        do {
            n = recv(state_->socket, state_->buf.data(), state_->buf.size(), 0);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            return std::nullopt;
        }
        if (static_cast<std::size_t>(n) < header) {
            throw std::runtime_error("a frame of " + std::to_string(n) + " bytes is shorter than its header");
        }
        return Frame{little(state_->buf.data()), little(state_->buf.data() + 8),
                     state_->buf.substr(header, static_cast<std::size_t>(n) - header)};
    }
    v1::CoreFrame f;
    if (state_->frames->pop(f) != Queue::Popped::frame) {
        throw Ended();
    }
    if (f.has_completion()) {
        return std::nullopt;
    }
    if (f.has_refusal()) {
        throw_refusal(f.refusal());
    }
    const auto &d = f.delivery();
    return Frame{d.sequence(), d.sent_at(), d.payload()};
}

void Delivery::release(std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_stream_unsubscribe()->set_delivery(state_->id);
    auto let_go = [this] {
        if (state_->socket >= 0) {
            ::shutdown(state_->socket, SHUT_RDWR);
        }
        if (state_->frames) {
            state_->a->forget(state_->id);
        }
    };
    try {
        state_->a->call(std::move(r), wait);
    } catch (...) {
        let_go();
        throw;
    }
    let_go();
}

Attachment::Attachment(std::shared_ptr<State> state) : state_(std::move(state)) {}

Attachment::~Attachment() { close(); }

std::unique_ptr<Attachment> Attachment::attach(const std::string &address, const Options &options,
                                               std::chrono::milliseconds wait) {
    auto s = std::make_shared<State>();
    if (!address.empty() && address[0] == '/') {
        if (options.use_tls) {
            throw std::runtime_error(address +
                                     " is a local socket, and a local socket is not reached over TLS");
        }
        s->channel = dial(address);
    } else if (options.use_tls) {
        if (options.roots.empty()) {
            throw std::runtime_error("TLS was asked with no roots, and the Core's chain is verified only "
                                     "against the roots its caller supplies");
        }
        int fd = -1;
        s->pump = tls::connect(address, options.roots, options.server_name, wait, fd);
        s->channel = grpc::CreateInsecureChannelFromFd(address, fd);
    } else {
        s->channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    }
    s->stub = v1::Interface::NewStub(s->channel);
    s->stream = s->stub->Attach(&s->context);
    if (options.credential) {
        s->count = 1;
        s->enroll(authenticating);
        v1::ClientFrame f;
        f.set_call(authenticating);
        f.mutable_request()->set_version(static_cast<std::uint32_t>(contract()));
        *f.mutable_request()->mutable_authenticate() = *options.credential;
        s->stream->Write(f);
    }
    s->reader = std::thread([raw = s.get()] { raw->receive(); });

    // Whatever fails from here lets the connection go before it is said.
    auto fail = [&s]() {
        s->context.TryCancel();
        if (s->reader.joinable()) {
            s->reader.join();
        }
    };
    v1::CoreFrame f;
    switch (s->opening->pop(f, std::chrono::steady_clock::now() + wait)) {
    case Queue::Popped::timed_out:
        fail();
        throw std::runtime_error("the channel at " + address + " did not open within " +
                                 std::to_string(wait.count()) + " ms");
    case Queue::Popped::closed:
        fail();
        throw_status(s->status);
    case Queue::Popped::frame:
        break;
    }
    if (!f.has_opening()) {
        fail();
        throw std::runtime_error("the attachment opened with something other than an opening");
    }
    const v1::Opening &op = f.opening();
    if (static_cast<int>(op.version()) != contract()) {
        fail();
        throw Refusal("compat.unsupported", "this library speaks the interface contract's version " +
                                                std::to_string(contract()) + ", and the Core " +
                                                std::to_string(op.version()));
    }
    s->picture = op.picture();
    s->version = static_cast<int>(op.version());
    if (!op.subscription().empty()) {
        // The standing subscription is the attachment's own and ends with it, so it holds no share of it.
        auto st = std::make_shared<Subscription::State>();
        st->id = op.subscription();
        st->frames = s->enroll(op.subscription());
        st->standing = true;
        s->standing = std::make_unique<Subscription>(st);
    }
    std::unique_ptr<Attachment> a(new Attachment(s));
    if (options.credential) {
        std::shared_ptr<Queue> q;
        {
            std::lock_guard<std::mutex> hold(s->mu);
            auto it = s->calls.find(authenticating);
            q = it != s->calls.end() ? it->second : nullptr;
        }
        if (!q) {
            throw Ended();
        }
        v1::Response resp = s->first(authenticating, *q, wait);
        s->forget(authenticating);
        s->established = resp.authenticate();
    }
    return a;
}

void Attachment::close() {
    if (state_) {
        // The standing subscription holds the state too; it is the attachment's, and goes with it.
        state_->close();
    }
}

const v1::Snapshot &Attachment::picture() const { return state_->picture; }

Subscription *Attachment::standing() const { return state_->standing.get(); }

int Attachment::version() const { return state_->version; }

const v1::Authenticated *Attachment::established() const {
    return state_->established ? &*state_->established : nullptr;
}

v1::Authenticated Attachment::authenticate(std::string_view account, std::string_view secret,
                                           std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_authenticate()->mutable_password()->set_account(std::string(account));
    r.mutable_authenticate()->mutable_password()->set_secret(std::string(secret));
    return state_->call(std::move(r), wait).authenticate();
}

v1::Authenticated Attachment::authenticate_token(std::string_view token, std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_authenticate()->set_token(std::string(token));
    return state_->call(std::move(r), wait).authenticate();
}

std::vector<v1::Record> Attachment::read(std::string_view kind, std::string_view identity,
                                         std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_read()->set_kind(std::string(kind));
    r.mutable_read()->set_identity(std::string(identity));
    const v1::Response resp = state_->call(std::move(r), wait);
    return {resp.read().records().begin(), resp.read().records().end()};
}

std::unique_ptr<Subscription> Attachment::subscribe(const v1::Filter &filter,
                                                    std::chrono::milliseconds wait) {
    v1::Request r;
    *r.mutable_subscribe()->mutable_filter() = filter;
    auto [id, q] = state_->issue(std::move(r));
    v1::Response resp;
    try {
        resp = state_->first(id, *q, wait);
    } catch (...) {
        state_->forget(id);
        throw;
    }
    auto st = std::make_shared<Subscription::State>();
    st->a = state_;
    st->id = id;
    st->frames = q;
    st->pending = resp.subscribe();
    return std::make_unique<Subscription>(st);
}

void Attachment::confirm(std::string_view subscription, std::uint64_t sequence,
                         std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_confirm()->set_subscription(std::string(subscription));
    r.mutable_confirm()->set_sequence(sequence);
    state_->call(std::move(r), wait);
}

v1::Acknowledged Attachment::command(std::string_view unit, std::string_view type, std::string_view payload,
                                     std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_command()->set_unit(std::string(unit));
    r.mutable_command()->set_type(std::string(type));
    r.mutable_command()->set_payload(std::string(payload));
    return state_->call(std::move(r), wait).command();
}

std::string Attachment::ask(std::string_view unit, std::string_view type, std::string_view payload,
                            std::chrono::milliseconds wait) {
    return ask_caused({}, unit, type, payload, wait);
}

std::string Attachment::ask_caused(std::string_view cause, std::string_view unit, std::string_view type,
                                   std::string_view payload, std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_query()->set_cause(std::string(cause));
    r.mutable_query()->set_unit(std::string(unit));
    r.mutable_query()->set_type(std::string(type));
    r.mutable_query()->set_payload(std::string(payload));
    return state_->call(std::move(r), wait).query().payload();
}

v1::Acknowledged Attachment::start_stream(std::string_view unit, std::string_view stream,
                                          std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_stream_start()->set_unit(std::string(unit));
    r.mutable_stream_start()->set_stream(std::string(stream));
    return state_->call(std::move(r), wait).stream_start();
}

v1::Acknowledged Attachment::stop_stream(std::string_view unit, std::string_view stream,
                                         std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_stream_stop()->set_unit(std::string(unit));
    r.mutable_stream_stop()->set_stream(std::string(stream));
    return state_->call(std::move(r), wait).stream_stop();
}

std::unique_ptr<Delivery> Attachment::subscribe_stream(std::string_view unit, std::string_view stream,
                                                       std::chrono::milliseconds wait) {
    return subscribe_stream_reading(
        unit, stream, {v1::UnitStream::READS_STREAM_SOCKET, v1::UnitStream::READS_SHARED_OBJECT}, wait);
}

std::unique_ptr<Delivery>
Attachment::subscribe_stream_reading(std::string_view unit, std::string_view stream,
                                     const std::vector<v1::UnitStream::Reads> &reads,
                                     std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_stream_subscribe()->set_unit(std::string(unit));
    r.mutable_stream_subscribe()->set_stream(std::string(stream));
    for (auto read : reads) {
        r.mutable_stream_subscribe()->add_reads(read);
    }
    const v1::Delivering d = state_->call(std::move(r), wait).stream_subscribe();
    auto st = std::make_shared<Delivery::State>();
    st->a = state_;
    st->id = d.delivery();
    st->flowing = d.flowing();
    switch (d.arrives_case()) {
    case v1::Delivering::kSocket: {
        sockaddr_un addr{};
        if (d.socket().size() >= sizeof addr.sun_path) {
            throw std::runtime_error("the delivery's socket " + d.socket() + " is too long a path to reach");
        }
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, d.socket().data(), d.socket().size());
        st->socket = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (st->socket < 0 || ::connect(st->socket, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
            throw std::runtime_error("the delivery's socket " + d.socket() +
                                     " cannot be reached: " + std::strerror(errno));
        }
        st->buf.resize(packet);
        {
            std::lock_guard<std::mutex> hold(state_->mu);
            state_->deliveries.push_back(st);
        }
        break;
    }
    case v1::Delivering::kStreamSocket:
    case v1::Delivering::kShared: {
        const std::string &path =
            d.arrives_case() == v1::Delivering::kShared ? d.shared() : d.stream_socket();
        sockaddr_un addr{};
        if (path.size() >= sizeof addr.sun_path) {
            throw std::runtime_error("the delivery's socket " + path + " is too long a path to reach");
        }
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.data(), path.size());
        st->socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (st->socket < 0 || ::connect(st->socket, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
            throw std::runtime_error("the delivery's socket " + path +
                                     " cannot be reached: " + std::strerror(errno));
        }
        st->shared = d.arrives_case() == v1::Delivering::kShared;
        st->lengthed = !st->shared;
        st->arrives = st->shared ? "shared" : "stream_socket";
        {
            std::lock_guard<std::mutex> hold(state_->mu);
            state_->deliveries.push_back(st);
        }
        break;
    }
    case v1::Delivering::kConnection:
        // Enrolled by the reader as the answer arrived, so no frame of it was missed.
        st->arrives = "connection";
        st->frames = state_->enroll(st->id);
        break;
    default:
        throw std::runtime_error("the delivery " + st->id + " arrives where this projection cannot read it");
    }
    return std::make_unique<Delivery>(st);
}

v1::Reclaimed Attachment::reclaim(std::chrono::milliseconds wait) {
    v1::Request r;
    r.mutable_reclaim();
    return state_->call(std::move(r), wait).reclaim();
}

// ---- a stream followed ----

namespace {

// Whether a refusal says the unit is not there yet — not running, or running with no Session — which a
// follow waits out.
bool returning(const Refusal &r) { return r.code() == "unit.not_running" || r.code() == "unit.no_session"; }

} // namespace

struct Follow::State {
    Attachment *a;
    std::string unit, stream;
    std::vector<v1::UnitStream::Reads> reads;
    std::unique_ptr<Delivery> current;
    std::string last;
    bool closed = false;

    // Starts the stream, where it may, and subscribes to it, until it succeeds, the unit's absence aside.
    void subscribe() {
        for (;;) {
            bool there = true;
            try {
                a->start_stream(unit, stream);
            } catch (const Refusal &r) {
                // A start refused for any other reason leaves a stream that may flow anyway: the
                // subscription says.
                there = !returning(r);
            }
            if (there) {
                try {
                    current = a->subscribe_stream_reading(unit, stream, reads);
                    return;
                } catch (const Refusal &r) {
                    if (!returning(r)) {
                        throw;
                    }
                }
            }
            std::this_thread::sleep_for(follow_retry);
        }
    }
};

Follow::Follow(std::unique_ptr<State> state) : state_(std::move(state)) {}

Follow::~Follow() {
    try {
        close();
    } catch (...) {
    }
}

const std::string &Follow::delivery() const { return state_->last; }

std::optional<Frame> Follow::next() {
    for (;;) {
        if (state_->closed || !state_->current) {
            return std::nullopt;
        }
        std::optional<Frame> f;
        try {
            f = state_->current->next();
        } catch (const Refusal &r) {
            if (!returning(r)) {
                throw;
            }
        }
        if (f) {
            state_->last = state_->current->id();
            return f;
        }
        // The delivery ended with the unit's Session: what was opened for it is let go, and the stream
        // followed again.
        try {
            state_->current->release();
        } catch (const Refusal &) {
        }
        state_->current.reset();
        state_->subscribe();
    }
}

void Follow::close() {
    if (state_->closed) {
        return;
    }
    state_->closed = true;
    if (auto d = std::move(state_->current)) {
        d->release();
    }
}

std::unique_ptr<Follow> Attachment::follow(std::string_view unit, std::string_view stream,
                                           const std::vector<v1::UnitStream::Reads> &reads) {
    auto s = std::make_unique<Follow::State>();
    s->a = this;
    s->unit = std::string(unit);
    s->stream = std::string(stream);
    s->reads = reads.empty() ? std::vector<v1::UnitStream::Reads>{v1::UnitStream::READS_STREAM_SOCKET,
                                                                  v1::UnitStream::READS_SHARED_OBJECT}
                             : reads;
    s->subscribe();
    return std::unique_ptr<Follow>(new Follow(std::move(s)));
}

} // namespace yoke::iface
