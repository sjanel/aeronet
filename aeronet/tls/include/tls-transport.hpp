#pragma once

#include <openssl/ssl.h>

#include <cstddef>
#include <memory>
#include <string_view>

#include "transport.hpp"

namespace aeronet {

// TLS transport (OpenSSL). Implementation will live in tls-transport.cpp.
class TlsTransport : public ITransport {
 public:
  using SslPtr = std::unique_ptr<SSL, void (*)(SSL*)>;

  explicit TlsTransport(SslPtr sslPtr) : _ssl(std::move(sslPtr)) {}

  std::size_t read(char* buf, std::size_t len, TransportHint& want) override;

  std::size_t write(std::string_view data, TransportHint& want) override;

  [[nodiscard]] bool handshakeDone() const noexcept override;

  // Perform best-effort bidirectional TLS shutdown (non-blocking). Safe to call multiple times.
  void shutdown() noexcept;

  [[nodiscard]] SSL* rawSsl() const noexcept { return _ssl.get(); }

  static void logErrorIfAny();

 private:
  SslPtr _ssl;
  bool _handshakeDone{false};
};

}  // namespace aeronet
