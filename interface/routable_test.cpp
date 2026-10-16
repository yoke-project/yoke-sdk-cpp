#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.hpp"
#include "core.hpp"
#include "tls.hpp"
#include "yoke/interface.hpp"

using namespace std::chrono_literals;
namespace yi = yoke::iface;

namespace {

// A root a client may trust, which issued itself for 127.0.0.1: its certificate and its key, in PEM.
struct Root {
    std::string cert, key, pem;
};

Root make_root(const std::string &name) {
    std::string dir = check::dir();
    std::string cert = dir + "/" + name + ".crt", key = dir + "/" + name + ".key";
    std::string command =
        "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 -subj /CN=" + name +
        " -addext subjectAltName=IP:127.0.0.1 -keyout " + key + " -out " + cert + " 2>/dev/null";
    REQUIRE(std::system(command.c_str()) == 0, "openssl made no certificate");
    return Root{cert, key, check::read(cert)};
}

// What one connection negotiated, as the Core read it.
struct Negotiated {
    int version = 0;
    std::string alpn;
    bool client_certificate = false;
};

// A Core's interface surface on loopback over TLS, presenting the chain root issued, at most at max: the
// handshake is OpenSSL's, and what it carries is relayed to the service, served on a local socket.
class Tls {
  public:
    Tls(grpc::Service &service, const Root &root, int max) {
        ctx_ = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_max_proto_version(ctx_, max);
        REQUIRE(SSL_CTX_use_certificate_chain_file(ctx_, root.cert.c_str()) == 1 &&
                    SSL_CTX_use_PrivateKey_file(ctx_, root.key.c_str(), SSL_FILETYPE_PEM) == 1,
                "the Core's chain could not be loaded");
        SSL_CTX_set_alpn_select_cb(
            ctx_,
            [](SSL *, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
               unsigned int inlen, void *) {
                static const unsigned char h2[] = {2, 'h', '2'};
                unsigned char *selected = nullptr;
                if (SSL_select_next_proto(&selected, outlen, h2, sizeof h2, in, inlen) !=
                    OPENSSL_NPN_NEGOTIATED) {
                    return SSL_TLSEXT_ERR_ALERT_FATAL;
                }
                *out = selected;
                return SSL_TLSEXT_ERR_OK;
            },
            nullptr);
        static int served = 0;
        local_ = check::dir() + "/tls-" + std::to_string(++served) + ".sock";
        grpc::ServerBuilder b;
        b.AddListeningPort("unix:" + local_, grpc::InsecureServerCredentials());
        b.RegisterService(&service);
        server_ = b.BuildAndStart();
        REQUIRE(server_ != nullptr, "the Core could not be served at ", local_);
        listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof addr;
        REQUIRE(bind(listener_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 &&
                    listen(listener_, 8) == 0 &&
                    getsockname(listener_, reinterpret_cast<sockaddr *>(&addr), &len) == 0,
                "the Core could not listen");
        address_ = "127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
        accepting_ = std::thread([this] { accept_all(); });
    }

    ~Tls() {
        stop_ = true;
        shutdown(listener_, SHUT_RDWR);
        accepting_.join();
        close(listener_);
        server_->Shutdown(std::chrono::system_clock::now() + 1s);
        pumps_.clear();
        SSL_CTX_free(ctx_);
    }

    const std::string &address() const { return address_; }

    std::vector<Negotiated> negotiated() {
        std::lock_guard<std::mutex> hold(mu_);
        return negotiated_;
    }

  private:
    void accept_all() {
        while (!stop_) {
            pollfd p{listener_, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) {
                continue;
            }
            int net = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
            if (net < 0) {
                continue;
            }
            SSL *ssl = SSL_new(ctx_);
            SSL_set_fd(ssl, net);
            if (SSL_accept(ssl) != 1) {
                // Refused at the handshake: nothing reaches the surface.
                ERR_clear_error();
                SSL_free(ssl);
                close(net);
                continue;
            }
            Negotiated n;
            n.version = SSL_version(ssl);
            const unsigned char *alpn = nullptr;
            unsigned int length = 0;
            SSL_get0_alpn_selected(ssl, &alpn, &length);
            n.alpn.assign(reinterpret_cast<const char *>(alpn), length);
            n.client_certificate = SSL_get0_peer_certificate(ssl) != nullptr;
            int relay = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strcpy(addr.sun_path, local_.c_str());
            if (connect(relay, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
                close(relay);
                SSL_free(ssl);
                close(net);
                continue;
            }
            std::lock_guard<std::mutex> hold(mu_);
            negotiated_.push_back(n);
            pumps_.push_back(std::make_unique<yi::tls::Pump>(ssl, net, relay));
        }
    }

    SSL_CTX *ctx_ = nullptr;
    std::unique_ptr<grpc::Server> server_;
    int listener_ = -1;
    std::string address_, local_;
    std::thread accepting_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<Negotiated> negotiated_;
    std::vector<std::unique_ptr<yi::tls::Pump>> pumps_;
};

// A Core that establishes its caller before it opens: it reads the first frame, accepts `alice` with
// `hunter2x` or the token it issued, and refuses anything else `auth.invalid`.
class Routable : public iv1::Interface::Service {
  public:
    std::string token;

    grpc::Status Attach(grpc::ServerContext *,
                        grpc::ServerReaderWriter<iv1::CoreFrame, iv1::ClientFrame> *stream) override {
        iv1::CoreFrame open;
        *open.mutable_opening() = opening_at(1);
        if (token.empty()) {
            stream->Write(open);
            iv1::ClientFrame f;
            while (stream->Read(&f)) {
            }
            return grpc::Status::OK;
        }
        iv1::ClientFrame f;
        if (!stream->Read(&f)) {
            return grpc::Status::OK;
        }
        {
            std::lock_guard<std::mutex> hold(mu);
            firsts.push_back(f);
        }
        const auto &auth = f.request().authenticate();
        bool password = auth.has_password() && auth.password().account() == "alice" &&
                        auth.password().secret() == "hunter2x";
        if (!password && auth.token() != token) {
            iv1::Refusal r;
            r.set_code("auth.invalid");
            r.set_message("the credential establishes nobody");
            return refused(grpc::StatusCode::UNAUTHENTICATED, r);
        }
        stream->Write(open);
        stream->Write(event_at(5));
        iv1::Response resp;
        resp.mutable_authenticate()->set_account("alice");
        resp.mutable_authenticate()->set_token(token);
        resp.mutable_authenticate()->mutable_expires()->set_seconds(1800000000);
        stream->Write(answered(f.call(), resp));
        while (stream->Read(&f)) {
        }
        return grpc::Status::OK;
    }

    std::mutex mu;
    std::vector<iv1::ClientFrame> firsts;
};

bool contains(const std::string &s, const std::string &sub) { return s.find(sub) != std::string::npos; }

std::string failure_of(const std::function<void()> &f) {
    try {
        f();
    } catch (const std::exception &e) {
        return e.what();
    }
    return "";
}

// std: yoke-sdk-cpp:across-a-routable-address.01
void test_the_library_dials_tls13_against_the_callers_roots() {
    Root a = make_root("a"), b = make_root("b");

    Routable plain;
    Tls good(plain, a, TLS1_3_VERSION);
    std::string why =
        failure_of([&] { yi::Attachment::attach(good.address(), yi::Options::tls(a.pem), 3s)->close(); });
    REQUIRE(why.empty(), "attaching over TLS with the right roots: ", why);
    auto n = good.negotiated();
    REQUIRE(n.size() == 1 && n[0].version == TLS1_3_VERSION && n[0].alpn == "h2" && !n[0].client_certificate,
            "the Core read ", n.size(), " connections, the first at version ", n.empty() ? 0 : n[0].version,
            " with ALPN '", n.empty() ? "" : n[0].alpn, "'");

    Routable plain2, plain3;
    Tls old(plain2, a, TLS1_2_VERSION);
    REQUIRE(!failure_of([&] { yi::Attachment::attach(old.address(), yi::Options::tls(a.pem), 3s); }).empty(),
            "a Core capped at TLS 1.2 was attached");
    Tls other(plain3, b, TLS1_3_VERSION);
    REQUIRE(
        !failure_of([&] { yi::Attachment::attach(other.address(), yi::Options::tls(a.pem), 3s); }).empty(),
        "a Core presenting a chain the roots do not hold was attached");
    REQUIRE(old.negotiated().empty() && other.negotiated().empty(),
            "a refused handshake reached the Core's surface");

    why = failure_of([] { yi::Attachment::attach("127.0.0.1:1", yi::Options::tls(""), 3s); });
    REQUIRE(contains(why, "roots"), "no roots was answered '", why, "'");
    why = failure_of(
        [&] { yi::Attachment::attach(check::dir() + "/panel.sock", yi::Options::tls(a.pem), 3s); });
    REQUIRE(contains(why, "local socket"), "TLS on a local socket was answered '", why, "'");
}

// std: yoke-sdk-cpp:across-a-routable-address.02
void test_the_credential_is_the_first_frame_and_its_answer_surfaced() {
    Root r = make_root("a");
    Routable c;
    c.token = "dG9rZW4tZm9yLWFsaWNl";
    Tls core(c, r, TLS1_3_VERSION);

    for (int i = 0; i < 2; i++) {
        yi::Options o = yi::Options::tls(r.pem);
        if (i == 0) {
            o.password("alice", "hunter2x");
        } else {
            o.token(c.token);
        }
        auto a = yi::Attachment::attach(core.address(), o, 3s);
        REQUIRE(a->version() == 1 && a->standing() != nullptr && a->picture().records_size() == 1,
                "attachment ", i, " did not surface the opening");
        const iv1::Authenticated *e = a->established();
        REQUIRE(e != nullptr && e->account() == "alice" && e->token() == c.token &&
                    e->expires().seconds() == 1800000000,
                "attachment ", i, " established ", e ? e->ShortDebugString() : std::string("nothing"));
        auto d = a->standing()->next();
        REQUIRE(d && d->event().seq() == 5, "attachment ", i,
                "'s standing subscription did not deliver the event 5");
        a->close();
    }
    std::vector<iv1::ClientFrame> firsts;
    {
        std::lock_guard<std::mutex> hold(c.mu);
        firsts = c.firsts;
    }
    REQUIRE(firsts.size() == 2, "the Core read ", firsts.size(), " first frames");
    for (const auto &f : firsts) {
        REQUIRE(yi::name(f.request()) == "authenticate" &&
                    static_cast<int>(f.request().version()) == yi::contract(),
                "a first frame is ", f.ShortDebugString());
    }
    REQUIRE(firsts[0].request().authenticate().password().account() == "alice" &&
                firsts[0].request().authenticate().password().secret() == "hunter2x",
            "the first attachment presented ", firsts[0].ShortDebugString());
    REQUIRE(firsts[1].request().authenticate().token() == c.token, "the second attachment presented ",
            firsts[1].ShortDebugString());

    std::string code;
    try {
        yi::Attachment::attach(core.address(), yi::Options::tls(r.pem).password("alice", "wrong"), 3s);
    } catch (const yoke::Refusal &refusal) {
        code = refusal.code();
    } catch (const std::exception &e) {
        code = std::string("not a refusal: ") + e.what();
    }
    REQUIRE(code == "auth.invalid", "a secret the host refuses was answered '", code, "'");
}

} // namespace

int main() {
    return check::run({
        {"yoke-sdk-cpp:across-a-routable-address.01", test_the_library_dials_tls13_against_the_callers_roots},
        {"yoke-sdk-cpp:across-a-routable-address.02",
         test_the_credential_is_the_first_frame_and_its_answer_surfaced},
    });
}
