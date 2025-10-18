#include "tls-transport.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstddef>
#include <string_view>

#include "log.hpp"
#include "transport.hpp"

namespace aeronet {

namespace {
inline bool isRetry(int code) { return code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE; }
}  // namespace

std::size_t TlsTransport::read(char* buf, std::size_t len, TransportHint& want) {
  want = TransportHint::None;

  std::size_t bytesRead{};

  if (!_handshakeDone) {
    const auto handshakeRet = ::SSL_do_handshake(_ssl.get());
    if (handshakeRet == 1) {
      _handshakeDone = true;
    } else {
      const auto err = ::SSL_get_error(_ssl.get(), handshakeRet);
      if (isRetry(err)) {
        want = (err == SSL_ERROR_WANT_WRITE) ? TransportHint::WriteReady : TransportHint::ReadReady;
        return bytesRead;  // indicate would-block during handshake
      }
      // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
      if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        want = TransportHint::ReadReady;  // Default to read, SSL will tell us if it needs write
        return bytesRead;
      }
      logErrorIfAny();
      want = TransportHint::Error;
      return bytesRead;
    }
  }
  if (::SSL_read_ex(_ssl.get(), buf, len, &bytesRead) == 1) {
    // success
    return bytesRead;
  }

  // SSL_read_ex returned <=0. Determine why using SSL_get_error to decide whether this
  // indicates an orderly close (ZERO_RETURN), a retry condition (WANT_READ/WANT_WRITE),
  // or a transient/cryptic SYSCALL with errno==0 and no OpenSSL errors which should be
  // treated as non-fatal would-block.
  const auto err = ::SSL_get_error(_ssl.get(), 0);
  if (err == SSL_ERROR_ZERO_RETURN) {
    // Clean shutdown from the peer.
    return 0;
  }
  if (isRetry(err)) {
    want = (err == SSL_ERROR_WANT_WRITE) ? TransportHint::WriteReady : TransportHint::ReadReady;
    return 0;
  }

  // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
  if (err == SSL_ERROR_SYSCALL) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      want = TransportHint::ReadReady;
      return 0;
    }
    // Some platforms may present SSL_ERROR_SYSCALL with errno==0 and no OpenSSL errors
    // during non-blocking handshakes; treat this as a non-fatal would-block to avoid
    // prematurely closing the connection on transient EOF readings.
    if (errno == 0 && ::ERR_peek_error() == 0) {
      want = TransportHint::ReadReady;
      return 0;
    }
  }
  want = TransportHint::Error;
  return bytesRead;
}

std::size_t TlsTransport::write(std::string_view data, TransportHint& want) {
  want = TransportHint::None;
  std::size_t bytesWritten{};

  // Ensure handshake is done
  if (!_handshakeDone) {
    const int handshakeRet = ::SSL_do_handshake(_ssl.get());
    if (handshakeRet == 1) {
      _handshakeDone = true;
    } else {
      const int err = ::SSL_get_error(_ssl.get(), handshakeRet);
      if (isRetry(err)) {
        want = (err == SSL_ERROR_WANT_WRITE) ? TransportHint::WriteReady : TransportHint::ReadReady;
        return bytesWritten;  // no progress, treat like EAGAIN
      }
      // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
      if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        want = TransportHint::WriteReady;
        return bytesWritten;
      }
      want = TransportHint::Error;
      return bytesWritten;  // fatal
    }
  }

  // Avoid calling OpenSSL with a zero-length buffer. Some OpenSSL builds
  // treat a null/zero-length pointer as an invalid argument and return
  // 'bad length'. If there's nothing to write, simply return 0.
  if (data.empty()) {
    return 0;
  }

  if (::SSL_write_ex(_ssl.get(), data.data(), data.size(), &bytesWritten) == 0) {
    const auto err = ::SSL_get_error(_ssl.get(), 0);
    if (isRetry(err)) {
      want = (err == SSL_ERROR_WANT_WRITE) ? TransportHint::WriteReady : TransportHint::ReadReady;
      bytesWritten = 0;  // CRITICAL: return 0 so caller retries with same data!
      return bytesWritten;
    }

    // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
    if (err == SSL_ERROR_SYSCALL) {
      auto saved_errno = errno;
      if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
        want = TransportHint::WriteReady;
        bytesWritten = 0;
        return bytesWritten;  // retry with same data
      }
      if (saved_errno == 0 && ERR_peek_error() == 0) {
        want = TransportHint::WriteReady;
        bytesWritten = 0;
        return bytesWritten;
      }
    }

    logErrorIfAny();

    want = TransportHint::Error;
    bytesWritten = 0;
    return bytesWritten;
  }

  return bytesWritten;
}

bool TlsTransport::handshakeDone() const noexcept { return _handshakeDone; }

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
  auto rc = ::SSL_shutdown(ssl);
  if (rc == 0) {  // Need second invocation to try completing bidirectional shutdown.
    ::SSL_shutdown(ssl);
  }
}

void TlsTransport::logErrorIfAny() {
  auto errVal = ::ERR_get_error();
  if (errVal != 0) {
    char errBuf[256];
    ::ERR_error_string_n(errVal, errBuf, sizeof(errBuf));
    log::error("TLS transport OpenSSL error: {}", errBuf);
    while ((errVal = ::ERR_get_error()) != 0) {
      ::ERR_error_string_n(errVal, errBuf, sizeof(errBuf));
      log::error("TLS transport OpenSSL error: {}", errBuf);
    }
  }
}

}  // namespace aeronet
