#pragma once

#include <openssl/ssl.h>

#include <cstddef>
#include <memory>

#include "transport.hpp"

namespace aeronet {

// TLS transport (OpenSSL). Implementation will live in tls-transport.cpp.
class TlsTransport : public ITransport {
 public:
  using SslPtr = std::unique_ptr<SSL, void (*)(SSL*)>;

  explicit TlsTransport(SSL* ssl) : _ssl(ssl, SSL_free) {}

  ssize_t read(char* buf, std::size_t len, bool& wantRead, bool& wantWrite) override;

  ssize_t write(const char* buf, std::size_t len, bool& wantRead, bool& wantWrite) override;

  [[nodiscard]] bool handshakePending() const noexcept override;

  // Perform best-effort bidirectional TLS shutdown (non-blocking). Safe to call multiple times.
  void shutdown() noexcept;

  [[nodiscard]] SSL* rawSsl() const noexcept { return _ssl.get(); }

 private:
  SslPtr _ssl{nullptr, SSL_free};
  bool _handshakeDone{false};
};

}  // namespace aeronet
