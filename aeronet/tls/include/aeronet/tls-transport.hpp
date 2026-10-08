#pragma once

#include <openssl/ssl.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/native-handle.hpp"
#include "aeronet/tls-ktls.hpp"
#include "aeronet/transport.hpp"

namespace aeronet {

// TLS transport (OpenSSL). Implementation will live in tls-transport.cpp.
class TlsTransport final : public TransportBackend<TlsTransport, TransportKind::Tls>, public SocketTransportState {
 public:
  using SslPtr = std::unique_ptr<SSL, void (*)(SSL*)>;

  /// Sets the write modes of the SSL object that the transport relies on: SSL_write_ex returns once a record is
  /// written (partial writes), and a write that would block can be retried from another buffer with the same bytes
  /// (the gather write rebuilds its records in a stack buffer).
  TlsTransport(SslPtr sslPtr, uint32_t minBytesForZerocopy);

  /// Non-blocking read of decrypted data.
  /// When the SSL object reads ahead (server contexts), records are decrypted until `len` bytes are read or the socket
  /// would block, like a plain socket read: a short read then drained the socket, unless hasPendingReadData() tells
  /// otherwise. Without read-ahead, a read returns the data of one TLS record at most.
  TransportResult read(char* buf, std::size_t len);

  TransportResult write(std::string_view data);

  /// Ordered write of two buffers (see the gather write below).
  TransportResult write(std::string_view firstBuf, std::string_view secondBuf);

  /// Ordered gather write.
  /// With kTLS send, the buffers are sent in a single gather write (the kernel builds full records across them).
  /// Otherwise, small buffers are copied with the start of the following ones into full TLS records instead of being
  /// encrypted one by one, which would make a record (and a syscall) of each of them - like the 9-byte header of each
  /// HTTP/2 frame.
  TransportResult write(std::span<const std::string_view> buffers);

  [[nodiscard]] bool handshakeDone() const noexcept { return _handshakeDone; }

  /// Check if a read may return data, or report a condition, without the socket becoming readable again.
  /// Critical for edge-triggered epoll: after SSL_read_ex, OpenSSL may have read more ciphertext from the kernel than
  /// it returned as plaintext. The kernel won't re-trigger EPOLLIN for data already consumed from the socket buffer,
  /// so callers must poll this before returning to epoll_wait.
  [[nodiscard]] bool hasPendingReadData() const noexcept;

  // Perform best-effort bidirectional TLS shutdown (non-blocking). Safe to call multiple times.
  void shutdown() noexcept;

  [[nodiscard]] SSL* rawSsl() const noexcept { return _ssl.get(); }

  void logErrorIfAny() const;

  /// Attempt to enable kTLS send offload. Call once after handshake completion.
  /// Once enabled, application data bypasses OpenSSL: it is written in clear on the socket, which the kernel encrypts.
  KtlsEnableResult enableKtlsSend();

  /// Returns true if kTLS send was successfully enabled (kernel handles encryption for sendfile).
  [[nodiscard]] bool isKtlsSendEnabled() const noexcept { return _ktlsResult == KtlsEnableResult::Enabled; }

  /// Attempt to enable zerocopy (MSG_ZEROCOPY) on the kTLS socket.
  /// Only effective when kTLS send is enabled. Call after enableKtlsSend().
  /// Returns true if zerocopy was enabled or already enabled.
  bool enableZerocopy() noexcept;

  /// Store the underlying socket fd for zerocopy operations.
  /// enableKtlsSend() also stores it, from the SSL object, once kTLS send is enabled.
  void setUnderlyingFd(NativeHandle fd) noexcept { _fd = fd; }

  /// Get the underlying socket fd.
  [[nodiscard]] NativeHandle underlyingFd() const noexcept { return _fd; }

 private:
  // What stopped the last read decrypting records until its buffer was full (read-ahead SSL objects only).
  enum class ReadStop : std::uint8_t {
    None,     // the buffer was filled, or no read yet: OpenSSL's buffered data tells if more is pending
    Drained,  // the socket would block: OpenSSL holds at most an incomplete record, that only new bytes can complete
    Pending,  // a condition the next SSL_read_ex reports again (peer close_notify, write needed)
    Failed,   // a fatal error, already logged: the next read reports it
  };

  TransportHint handshake(TransportHint want);

  // Maps a failed SSL_read_ex to the hint of a read returning no data (None for an orderly close).
  [[nodiscard]] TransportHint readFailureHint() const;

  // SSL_write_ex of data, mapped to a transport result.
  TransportResult sslWrite(std::string_view data);

  SslPtr _ssl;
  // Applied to the zerocopy state by enableZerocopy(), once kTLS send is known to be enabled.
  uint32_t _minBytesForZerocopy;
  bool _handshakeDone{false};
  bool _readUntilDrained;
  ReadStop _readStop{ReadStop::None};
  KtlsEnableResult _ktlsResult{KtlsEnableResult::Unknown};
};

}  // namespace aeronet
