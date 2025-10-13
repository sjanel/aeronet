#include "tls-transport.hpp"

#include <openssl/ssl.h>
#include <sys/types.h>

#include <cerrno>
#include <cstddef>
#include <limits>
#include <string_view>

namespace aeronet {

namespace {
inline bool isRetry(int code) { return code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE; }
}  // namespace

ssize_t TlsTransport::read(char* buf, std::size_t len, TransportWant& want) {
  want = TransportWant::None;
  if (!_handshakeDone) {
    const auto hr = SSL_do_handshake(_ssl.get());
    if (hr == 1) {
      _handshakeDone = true;
    } else {
      const auto err = SSL_get_error(_ssl.get(), hr);
      if (isRetry(err)) {
        want = (err == SSL_ERROR_WANT_WRITE) ? TransportWant::WriteReady : TransportWant::ReadReady;
        return -1;  // indicate would-block during handshake
      }
      // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
      if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        want = TransportWant::ReadReady;  // Default to read, SSL will tell us if it needs write
        return -1;
      }
      return -1;  // fatal
    }
  }
  const auto bytesRead = SSL_read(_ssl.get(), buf, static_cast<int>(len));
  if (bytesRead > 0) {
    return bytesRead;
  }
  if (bytesRead == 0) {
    return 0;  // close notify or orderly close
  }
  const auto err = SSL_get_error(_ssl.get(), bytesRead);
  if (isRetry(err)) {
    want = (err == SSL_ERROR_WANT_WRITE) ? TransportWant::WriteReady : TransportWant::ReadReady;
    return -1;
  }
  // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
  if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    want = TransportWant::ReadReady;
    return -1;
  }
  return -1;  // fatal
}

ssize_t TlsTransport::write(std::string_view data, TransportWant& want) {
  want = TransportWant::None;

  // Ensure handshake is done
  if (!_handshakeDone) {
    const int hr = ::SSL_do_handshake(_ssl.get());
    if (hr == 1) {
      _handshakeDone = true;
    } else {
      const int err = ::SSL_get_error(_ssl.get(), hr);
      if (isRetry(err)) {
        want = (err == SSL_ERROR_WANT_WRITE) ? TransportWant::WriteReady : TransportWant::ReadReady;
        return 0;  // no progress, treat like EAGAIN
      }
      // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
      if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        want = TransportWant::WriteReady;
        return 0;
      }
      return -1;  // fatal
    }
  }

  // SSL_write requires that on WANT_READ/WANT_WRITE, we retry with the EXACT same buffer and length.
  // We cannot split into chunks and retry with different data - OpenSSL tracks this internally.
  // Therefore, we attempt to write the entire data in one call (up to INT_MAX).

  if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    // For very large writes, we'd need to handle this differently, but for now we limit
    data = data.substr(0, std::numeric_limits<int>::max());
  }

  const int res = ::SSL_write(_ssl.get(), data.data(), static_cast<int>(data.size()));
  if (res > 0) {
    return res;
  }

  const int err = ::SSL_get_error(_ssl.get(), res);
  if (isRetry(err)) {
    want = (err == SSL_ERROR_WANT_WRITE) ? TransportWant::WriteReady : TransportWant::ReadReady;
    return 0;  // CRITICAL: return 0 so caller retries with same data!
  }

  // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
  if (err == SSL_ERROR_SYSCALL) {
    int saved_errno = errno;
    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
      want = TransportWant::WriteReady;
      return 0;  // retry with same data
    }
  }

  // Fatal error
  return -1;
}

bool TlsTransport::handshakePending() const noexcept { return !_handshakeDone; }

void TlsTransport::shutdown() noexcept {
  auto* ssl = _ssl.get();
  if (ssl == nullptr) {
    return;
  }
  // OpenSSL SSL_shutdown semantics (simplified):
  //  - First call attempts to send our "close_notify" alert. Return values:
  //      1 : Bidirectional shutdown already complete (we previously received peer's close_notify).
  //      0 : Our close_notify sent, but peer's close_notify not yet seen (need a second call).
  //     <0 : Error or needs retry (SSL_ERROR_WANT_READ / SSL_ERROR_WANT_WRITE for non-blocking I/O).
  //  - A second call (only when the first returned 0) lets OpenSSL process an already received peer
  //    close_notify (if it arrived between calls) or indicates that we still need to read to finish.
  //
  // In this transport we issue at most two immediate calls as a best-effort graceful shutdown and then
  // rely on the outer layer closing the underlying socket. We intentionally ignore WANT_READ / WANT_WRITE
  // here for simplicity; a fully asynchronous graceful close would capture those conditions and defer
  // the second call until the socket becomes readable/writable.
  auto rc = SSL_shutdown(ssl);
  if (rc == 0) {  // Need second invocation to try completing bidirectional shutdown.
    (void)SSL_shutdown(ssl);
  }
}

}  // namespace aeronet
