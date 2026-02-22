#pragma once

#include <openssl/ssl.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/tls-ktls.hpp"
#include "aeronet/transport.hpp"

namespace aeronet {

// TLS transport (OpenSSL). Implementation will live in tls-transport.cpp.
class TlsTransport final : public ITransport {
 public:
  using SslPtr = std::unique_ptr<SSL, void (*)(SSL*)>;

  TlsTransport(SslPtr sslPtr, uint32_t minBytesForZerocopy)
      : ITransport(-1, minBytesForZerocopy), _ssl(std::move(sslPtr)) {}

  TransportResult read(char* buf, std::size_t len) override;

  TransportResult write(std::string_view data) override;

  [[nodiscard]] bool handshakeDone() const noexcept override { return _handshakeDone; }

  // Perform best-effort bidirectional TLS shutdown (non-blocking). Safe to call multiple times.
  void shutdown() noexcept;

  [[nodiscard]] SSL* rawSsl() const noexcept { return _ssl.get(); }

  void logErrorIfAny() const noexcept;

  /// Attempt to enable kTLS send offload. Call once after handshake completion.
  KtlsEnableResult enableKtlsSend();

  /// Returns true if kTLS send was successfully enabled (kernel handles encryption for sendfile).
  [[nodiscard]] bool isKtlsSendEnabled() const noexcept { return _ktlsResult == KtlsEnableResult::Enabled; }

  /// Attempt to enable zerocopy (MSG_ZEROCOPY) on the kTLS socket.
  /// Only effective when kTLS send is enabled. Call after enableKtlsSend().
  /// Returns true if zerocopy was enabled or already enabled.
  bool enableZerocopy() noexcept;

  /// Store the underlying socket fd for zerocopy operations.
  /// Called after SSL_set_fd to cache the fd for direct socket I/O when kTLS is active.
  void setUnderlyingFd(int fd) noexcept { _fd = fd; }

  /// Get the underlying socket fd.
  [[nodiscard]] int underlyingFd() const noexcept { return _fd; }

 private:
  TransportHint handshake(TransportHint want);

  /// Internal write using zerocopy when kTLS + zerocopy are both enabled.
  TransportResult writeZerocopy(std::string_view data);

  SslPtr _ssl;
  bool _handshakeDone{false};
  KtlsEnableResult _ktlsResult{KtlsEnableResult::Unknown};
};

}  // namespace aeronet
