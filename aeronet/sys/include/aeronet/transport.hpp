#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/native-handle.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "aeronet/zerocopy.hpp"

namespace aeronet {

class File;

// Indicates what the transport layer needs to proceed after a non-blocking I/O operation returns EAGAIN/WANT.
enum class TransportHint : uint8_t {
  None,        // No special action needed (operation completed or fatal error)
  ReadReady,   // Need socket readable before operation can proceed (SSL_ERROR_WANT_READ)
  WriteReady,  // Need socket writable before operation can proceed (SSL_ERROR_WANT_WRITE)
  Error
};

// Base transport abstraction; allows transparent TLS or plain socket IO.
// TODO: check if we cannot simply make a unique Transport non virtual class with internal variants.
class ITransport {
 public:
  ITransport() noexcept = default;

  ITransport(NativeHandle fd, std::uint32_t minBytesForZerocopy) : _minBytesForZerocopy(minBytesForZerocopy), _fd(fd) {}

  virtual ~ITransport() = default;

  struct TransportResult {
    std::size_t bytesProcessed;  // bytes read for read operations, or written for write operations
    TransportHint want;          // indicates whether socket needs to be readable or writable for operation to proceed.
  };

  // Non-blocking read. Returns bytes read (>0), 0 on orderly close, -1 on EAGAIN/WANT (caller inspects want).
  // want: indicates whether socket needs to be readable or writable for operation to proceed.
  virtual TransportResult read(char* buf, std::size_t len) = 0;

  // Non-blocking write. Returns the number of bytes written. If 0, check the want parameter.
  // want: indicates whether socket needs to be readable or writable for operation to proceed.
  virtual TransportResult write(std::string_view data) = 0;

  // Non-blocking scatter write. Returns total bytes written across both buffers.
  // Default implementation calls write() twice; PlainTransport overrides with writev for efficiency.
  // want: indicates whether socket needs to be readable or writable for operation to proceed.
  virtual TransportResult write(std::string_view firstBuf, std::string_view secondBuf) {
    // First attempt to write the response head. Only if the head was fully
    // written do we proceed to write the body. This is important for TLS
    // transports where a write call may succeed and report a positive
    // "bytes written" value that is nevertheless smaller than the
    // requested buffer. In that partial-write case we must not start
    // sending the body bytes before the remaining head bytes have been
    // flushed, otherwise the client will see a corrupted/invalid response.
    TransportResult result = write(firstBuf);
    if (result.want != TransportHint::None) {
      // Transport indicated it needs readiness or error — caller will retry.
      return result;
    }

    // Only continue to body if the head was fully consumed.
    if (result.bytesProcessed < firstBuf.size()) {
      return result;
    }

    if (!secondBuf.empty()) {
      const auto [bytesWritten, want] = write(secondBuf);
      result.bytesProcessed += bytesWritten;
      result.want = want;
    }
    return result;
  }

  // Whether this transport can transfer a file region straight to the socket with a single zero-copy
  // syscall (sendfile / TransmitFile), bypassing a user-space read + write. True only for plain
  // (unencrypted) transports; a TLS transport must read + encrypt in user space (kTLS aside, which the
  // client does not set up), so it reports false and the caller falls back to read() + write().
  [[nodiscard]] virtual bool supportsSendfile() const noexcept { return false; }

  // Zero-copy transfer of up to `count` bytes from the open file `file` starting at `offset`, straight to
  // the socket. Only valid when supportsSendfile() is true (the default is a no-op guarded by that).
  // `offset` is advanced by the number of bytes actually sent. Returns the standard TransportResult:
  // `bytesProcessed` sent (may be a partial send), with `want` set to WriteReady when the socket send
  // buffer is full or to Error on a fatal failure - exactly like write().
  virtual TransportResult sendFile([[maybe_unused]] const File& file, [[maybe_unused]] std::size_t& offset,
                                   [[maybe_unused]] std::size_t count) {
    return {0, TransportHint::Error};
  }

  [[nodiscard]] virtual bool handshakeDone() const noexcept { return true; }

  /// Check if the transport has internally buffered data ready to read.
  /// For TLS, OpenSSL may hold decrypted data that the kernel socket no longer
  /// signals via epoll (critical with edge-triggered mode).
  [[nodiscard]] virtual bool hasPendingReadData() const noexcept { return false; }

  /// Poll for zerocopy completion notifications from the kernel error queue.
  /// Returns the number of completions processed.
  std::size_t pollZerocopyCompletions() noexcept { return PollZeroCopyCompletions(_fd, _zerocopyState); }

  /// Check if zerocopy is enabled on this transport.
  [[nodiscard]] bool isZerocopyEnabled() const noexcept { return _zerocopyState.enabled(); }

  /// Check if there are any outstanding zerocopy sends waiting for completion.
  [[nodiscard]] bool hasZerocopyPending() const noexcept { return _zerocopyState.pendingCompletions(); }

  /// Disable zerocopy for this transport (useful when buffer lifetimes are not stable,
  /// e.g. CONNECT tunneling that reuses read buffers).
  void disableZerocopy() noexcept { _zerocopyState.setEnabled(false); }

 protected:
  ZeroCopyState _zerocopyState{};
  std::uint32_t _minBytesForZerocopy{~0U};
  NativeHandle _fd{kInvalidHandle};
};

// Plain transport directly operates on a non-blocking fd.
// Supports optional MSG_ZEROCOPY for large payloads on Linux.
class PlainTransport final : public ITransport {
 public:
  PlainTransport(NativeHandle fd, ZerocopyMode zerocopyMode, uint32_t minBytesForZerocopy);

  TransportResult read(char* buf, std::size_t len) override;

  TransportResult write(std::string_view data) override;

  /// Scatter write using writev - single syscall for two buffers.
  TransportResult write(std::string_view firstBuf, std::string_view secondBuf) override;

  /// A plain socket can transfer a file region with the zero-copy sendfile(2) path.
  [[nodiscard]] bool supportsSendfile() const noexcept override { return true; }

  /// Zero-copy sendfile(2) of a file region straight to the socket (no user-space copy).
  TransportResult sendFile(const File& file, std::size_t& offset, std::size_t count) override;
};

}  // namespace aeronet