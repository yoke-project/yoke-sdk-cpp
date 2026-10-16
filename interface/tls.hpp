// TLS 1.3 under the interface library, done here rather than by gRPC: the gRPC this family builds on
// cannot hold a connection to TLS 1.3 alone, and a channel at a routable address takes nothing below it.
// The handshake is OpenSSL's; once it is done, a pump carries the plaintext between the TLS connection and
// one end of a socket pair, and gRPC speaks HTTP/2 on the other end as on any connected socket. Never
// installed.
#ifndef YOKE_INTERFACE_TLS_HPP
#define YOKE_INTERFACE_TLS_HPP

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <openssl/ssl.h>

namespace yoke::iface::tls {

// Carries bytes both ways between a TLS connection and a local socket, on a thread of its own, until
// either side ends; then ends the other.
class Pump {
  public:
    // Takes ssl, its connected socket and the local socket; all three are its own from here.
    Pump(SSL *ssl, int net, int local);
    // Stops the pump and closes what it holds.
    ~Pump();
    Pump(const Pump &) = delete;
    Pump &operator=(const Pump &) = delete;

  private:
    void run();

    SSL *ssl_;
    int net_, local_;
    int stop_[2] = {-1, -1};
    std::thread thread_;
};

// A TLS 1.3 connection to host:port: the Core's chain verified against roots, in PEM, and no others, for
// name — host where it is empty — with ALPN `h2` and no client certificate. Returns the pump carrying it,
// and sets fd to the socket gRPC is to speak on. A failure is thrown, naming what failed.
std::unique_ptr<Pump> connect(const std::string &address, const std::string &roots, const std::string &name,
                              std::chrono::milliseconds wait, int &fd);

// What OpenSSL last said, as one line.
std::string said();

} // namespace yoke::iface::tls

#endif
