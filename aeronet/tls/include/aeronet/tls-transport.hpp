#pragma once

#include <openssl/ssl.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/tls-ktls.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/zerocopy.hpp"

namespace aeronet {

// TLS transport (OpenSSL). Implementation will live in tls-transport.cpp.
class TlsTransport final : public ITransport {
 public:
  using SslPtr = std::unique_ptr<SSL, void (*)(SSL*)>;

  explicit TlsTransport(SslPtr sslPtr) : _ssl(std::move(sslPtr)) {}

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

  /// Check if zerocopy is enabled on this transport.
  [[nodiscard]] bool isZerocopyEnabled() const noexcept { return _zerocopyState.enabled; }

  /// Poll for zerocopy completion notifications from the kernel error queue.
  /// Returns the number of completions processed.
  std::size_t pollZerocopyCompletions() noexcept;

  /// Disable zerocopy for this transport.
  void disableZerocopy() noexcept;

  /// Check if there are any outstanding zerocopy sends waiting for completion.
  [[nodiscard]] bool hasZerocopyPending() const noexcept { return _zerocopyState.pendingCompletions; }

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
  int _fd{-1};  // cached fd for zerocopy operations
  bool _handshakeDone{false};
  KtlsEnableResult _ktlsResult{KtlsEnableResult::Unknown};
  ZeroCopyState _zerocopyState{};
};

}  // namespace aeronet
