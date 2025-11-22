#pragma once

#include <openssl/ssl.h>
#include <openssl/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/http-header.hpp"
#include "aeronet/test_util.hpp"

namespace aeronet::test {

// Lightweight RAII TLS client used in tests to reduce duplication.
// Features:
//  * Automatic OpenSSL context + SSL object creation
//  * Optional ALPN protocol list (vector of protocol strings)
//  * Optional in-memory client certificate/key (PEM)
//  * Disable verification by default (tests use self-signed server certs)
//  * Simple helpers to GET a path and read full response
//  * Accessors for handshake success, negotiated ALPN
//
// Not intended for production usage; minimal error handling for brevity.
class TlsClient {
 public:
  struct Options {
    std::vector<std::string> alpn;  // e.g. {"http/1.1"}
    bool verifyPeer{false};         // off for self-signed tests
    std::string clientCertPem;      // optional client cert (mTLS)
    std::string clientKeyPem;       // optional client key (mTLS)
  };

  // Constructor without custom options.
  explicit TlsClient(uint16_t port) : _port(port), _opts(), _cnx(_port) { init(); }

  // Constructor with explicit options.
  TlsClient(uint16_t port, Options options) : _port(port), _opts(std::move(options)), _cnx(_port) { init(); }

  TlsClient(const TlsClient&) = delete;
  TlsClient(TlsClient&&) noexcept = delete;
  TlsClient& operator=(const TlsClient&) = delete;
  TlsClient& operator=(TlsClient&&) noexcept = delete;

  ~TlsClient();

  [[nodiscard]] bool handshakeOk() const { return _handshakeOk; }

  // Send arbitrary bytes (only if handshake succeeded).
  bool writeAll(std::string_view data);

  // Read until close (or error). Returns accumulated data.
  std::string readAll();

  // Convenience: perform a GET request and read entire response.
  std::string get(std::string_view target, const std::vector<http::Header>& extraHeaders = {});

  [[nodiscard]] std::string_view negotiatedAlpn() const { return _negotiatedAlpn; }

 private:
  using SSLUniquePtr = std::unique_ptr<SSL, void (*)(SSL*)>;
  using SSL_CTXUniquePtr = std::unique_ptr<SSL_CTX, void (*)(SSL_CTX*)>;

  void init();

  void loadClientCertKey(SSL_CTX* ctx);

  // Wait for socket to be ready for reading or writing.
  // events: POLLIN for readable, POLLOUT for writable
  // Returns true if ready, false on timeout or error
  template <typename Duration>
  bool waitForSocketReady(short events, Duration timeout);

  uint16_t _port;
  Options _opts;
  bool _handshakeOk{false};
  std::string _negotiatedAlpn;
  aeronet::test::ClientConnection _cnx;
  // Initialize with valid deleter function pointers so default-constructed state is safe.
  SSL_CTXUniquePtr _ctx{nullptr, ::SSL_CTX_free};
  SSLUniquePtr _ssl{nullptr, ::SSL_free};
};
}  // namespace aeronet::test