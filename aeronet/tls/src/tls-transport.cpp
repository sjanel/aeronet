#include "tls-transport.hpp"

#include <openssl/ssl.h>  // SSL_*, SSL_read/write, handshake, shutdown, SSL_CTX, SSL_get_error
#include <sys/types.h>

#include <cerrno>
#include <cstring>

namespace aeronet {

namespace {
inline bool isRetry(int code) { return code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE; }
}  // namespace

ssize_t TlsTransport::read(char* buf, std::size_t len, bool& wantRead, bool& wantWrite) {
  wantRead = wantWrite = false;
  if (!_handshakeDone) {
    const auto hr = SSL_do_handshake(_ssl.get());
    if (hr == 1) {
      _handshakeDone = true;
    } else {
      const auto err = SSL_get_error(_ssl.get(), hr);
      if (isRetry(err)) {
        wantRead = (err == SSL_ERROR_WANT_READ);
        wantWrite = (err == SSL_ERROR_WANT_WRITE);
        return -1;  // indicate would-block during handshake
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
    wantRead = (err == SSL_ERROR_WANT_READ);
    wantWrite = (err == SSL_ERROR_WANT_WRITE);
    return -1;
  }
  return -1;  // fatal
}

ssize_t TlsTransport::write(const char* buf, std::size_t len, bool& wantRead, bool& wantWrite) {
  wantRead = wantWrite = false;
  if (!_handshakeDone) {
    const auto hr = SSL_do_handshake(_ssl.get());
    if (hr == 1) {
      _handshakeDone = true;
    } else {
      auto err = SSL_get_error(_ssl.get(), hr);
      if (isRetry(err)) {
        wantRead = (err == SSL_ERROR_WANT_READ);
        wantWrite = (err == SSL_ERROR_WANT_WRITE);
        return -1;
      }
      return -1;  // fatal
    }
  }
  const auto bytesWritten = SSL_write(_ssl.get(), buf, static_cast<int>(len));
  if (bytesWritten > 0) {
    return bytesWritten;
  }
  const auto err = SSL_get_error(_ssl.get(), bytesWritten);
  if (isRetry(err)) {
    wantRead = (err == SSL_ERROR_WANT_READ);
    wantWrite = (err == SSL_ERROR_WANT_WRITE);
    return -1;
  }
  return -1;  // fatal
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
