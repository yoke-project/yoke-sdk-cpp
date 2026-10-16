#include "tls.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace yoke::iface::tls {

namespace {

// How much either direction holds before it stops reading.
constexpr std::size_t held = 1 << 20;

void nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

} // namespace

std::string said() {
    unsigned long e = ERR_get_error();
    if (e == 0) {
        return "no further reason";
    }
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    ERR_clear_error();
    return buf;
}

Pump::Pump(SSL *ssl, int net, int local) : ssl_(ssl), net_(net), local_(local) {
    if (pipe2(stop_, O_CLOEXEC) != 0) {
        throw std::runtime_error(std::string("the pump cannot be made: ") + std::strerror(errno));
    }
    nonblocking(net_);
    nonblocking(local_);
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    thread_ = std::thread([this] { run(); });
}

Pump::~Pump() {
    [[maybe_unused]] ssize_t n = write(stop_[1], "", 1);
    if (thread_.joinable()) {
        thread_.join();
    }
    SSL_free(ssl_);
    close(net_);
    close(local_);
    close(stop_[0]);
    close(stop_[1]);
}

void Pump::run() {
    std::string out, in;     // out: from the local socket, for the connection; in: the other way
    bool local_open = true;  // the local side still sends
    bool want_write = false; // OpenSSL last asked to write before it could go on
    bool shut = false;       // the connection was told the local side is done
    for (;;) {
        pollfd p[3] = {
            {net_, static_cast<short>(POLLIN | (want_write || !out.empty() ? POLLOUT : 0)), 0},
            {local_,
             static_cast<short>((local_open && out.size() < held ? POLLIN : 0) | (!in.empty() ? POLLOUT : 0)),
             0},
            {stop_[0], POLLIN, 0}};
        if (poll(p, 3, -1) < 0 && errno != EINTR) {
            break;
        }
        if (p[2].revents) {
            break;
        }
        // What the local side sent, for the connection.
        if (local_open && out.size() < held) {
            char buf[16384];
            ssize_t n = read(local_, buf, sizeof buf);
            if (n > 0) {
                out.append(buf, static_cast<std::size_t>(n));
            } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                local_open = false;
            }
        }
        want_write = false;
        while (!out.empty()) {
            int n = SSL_write(ssl_, out.data(), static_cast<int>(std::min<std::size_t>(out.size(), 1 << 30)));
            if (n > 0) {
                out.erase(0, static_cast<std::size_t>(n));
                continue;
            }
            int e = SSL_get_error(ssl_, n);
            if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
                want_write = e == SSL_ERROR_WANT_WRITE;
                break;
            }
            return; // the connection failed: the local side sees its end when the pump is let go
        }
        if (!local_open && out.empty() && !shut) {
            shut = true;
            // The local side is done: say so on the connection, and let the peer end it.
            SSL_shutdown(ssl_);
            ::shutdown(net_, SHUT_WR);
        }
        // What the connection brought, for the local side.
        bool net_ended = false;
        while (in.size() < held) {
            char buf[16384];
            int n = SSL_read(ssl_, buf, sizeof buf);
            if (n > 0) {
                in.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            int e = SSL_get_error(ssl_, n);
            if (e == SSL_ERROR_WANT_READ) {
                break;
            }
            if (e == SSL_ERROR_WANT_WRITE) {
                want_write = true;
                break;
            }
            net_ended = true;
            break;
        }
        while (!in.empty()) {
            ssize_t n = write(local_, in.data(), in.size());
            if (n > 0) {
                in.erase(0, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            }
            return;
        }
        if (net_ended && in.empty()) {
            // The connection ended: the local side reads the end.
            // Nothing more arrives, and what the local side still sends has nowhere to go.
            ::shutdown(local_, SHUT_WR);
            return;
        }
    }
}

namespace {

// host and port from `host:port` or `[host]:port`.
void split(const std::string &address, std::string &host, std::string &port) {
    if (!address.empty() && address[0] == '[') {
        auto close = address.find(']');
        if (close == std::string::npos || close + 1 >= address.size() || address[close + 1] != ':') {
            throw std::runtime_error("the address " + address + " is not [host]:port");
        }
        host = address.substr(1, close - 1), port = address.substr(close + 2);
        return;
    }
    auto colon = address.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == address.size()) {
        throw std::runtime_error("the address " + address + " is not host:port");
    }
    host = address.substr(0, colon), port = address.substr(colon + 1);
}

int dial(const std::string &host, const std::string &port, std::chrono::milliseconds wait) {
    addrinfo hints{}, *found = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (int e = getaddrinfo(host.c_str(), port.c_str(), &hints, &found); e != 0) {
        throw std::runtime_error("the address " + host + ":" + port +
                                 " cannot be resolved: " + gai_strerror(e));
    }
    std::string why = "it resolves to nothing";
    int fd = -1;
    for (addrinfo *a = found; a != nullptr && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) {
            why = std::strerror(errno);
            continue;
        }
        timeval tv{static_cast<time_t>(wait.count() / 1000),
                   static_cast<suseconds_t>((wait.count() % 1000) * 1000)};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (::connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
            why = std::strerror(errno);
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(found);
    if (fd < 0) {
        throw std::runtime_error("the address " + host + ":" + port + " cannot be reached: " + why);
    }
    return fd;
}

} // namespace

std::unique_ptr<Pump> connect(const std::string &address, const std::string &roots, const std::string &name,
                              std::chrono::milliseconds wait, int &fd) {
    std::string host, port;
    split(address, host, port);
    const std::string expected = name.empty() ? host : name;

    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    if (!ctx) {
        throw std::runtime_error("TLS cannot be set up: " + said());
    }
    // TLS 1.3, and nothing below it.
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION);
    // The roots the caller supplied, and no others: the system's are never loaded.
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(roots.data(), static_cast<int>(roots.size())), BIO_free);
    int loaded = 0;
    while (X509 *cert = PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)) {
        X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx.get()), cert);
        X509_free(cert);
        loaded++;
    }
    ERR_clear_error();
    if (loaded == 0) {
        throw std::runtime_error(
            "the roots hold no certificate, and the Core's chain is verified only against the "
            "roots its caller supplies");
    }
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
    static const unsigned char h2[] = {2, 'h', '2'};
    SSL_CTX_set_alpn_protos(ctx.get(), h2, sizeof h2);

    int net = dial(host, port, wait);
    SSL *ssl = SSL_new(ctx.get());
    auto fail = [&](const std::string &why) {
        SSL_free(ssl);
        close(net);
        throw std::runtime_error("the TLS handshake with " + address + " failed: " + why);
    };
    if (!ssl) {
        fail(said());
    }
    X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
    in6_addr ip6;
    in_addr ip4;
    if (inet_pton(AF_INET, expected.c_str(), &ip4) == 1 || inet_pton(AF_INET6, expected.c_str(), &ip6) == 1) {
        X509_VERIFY_PARAM_set1_ip_asc(param, expected.c_str());
    } else {
        SSL_set_tlsext_host_name(ssl, expected.c_str());
        SSL_set1_host(ssl, expected.c_str());
    }
    SSL_set_fd(ssl, net);
    if (SSL_connect(ssl) != 1) {
        long verified = SSL_get_verify_result(ssl);
        fail(verified != X509_V_OK ? X509_verify_cert_error_string(verified) : said());
    }
    const unsigned char *selected = nullptr;
    unsigned int length = 0;
    SSL_get0_alpn_selected(ssl, &selected, &length);
    if (length != 2 || std::memcmp(selected, "h2", 2) != 0) {
        fail("the Core did not select the protocol h2");
    }
    // The handshake is bounded by wait; what follows is not.
    timeval none{0, 0};
    setsockopt(net, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
    setsockopt(net, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        fail(std::strerror(errno));
    }
    fd = pair[1];
    return std::make_unique<Pump>(ssl, net, pair[0]);
}

} // namespace yoke::iface::tls
