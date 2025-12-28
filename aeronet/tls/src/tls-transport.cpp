#include "aeronet/tls-transport.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstddef>
#include <string_view>

#include "aeronet/log.hpp"
#include "aeronet/tls-ktls.hpp"
#include "aeronet/transport.hpp"

namespace aeronet {

static_assert(EAGAIN == EWOULDBLOCK, "Add handling for EWOULDBLOCK if different from EAGAIN");

namespace {
inline bool isRetry(int code) { return code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE; }
}  // namespace

ITransport::TransportResult TlsTransport::read(char* buf, std::size_t len) {
  TransportResult ret{0, handshake(TransportHint::ReadReady)};
  if (ret.want != TransportHint::None) {
    return ret;  // indicate would-block during handshake
  }

  if (::SSL_read_ex(_ssl.get(), buf, len, &ret.bytesProcessed) == 1) [[likely]] {
    // success
    return ret;
  }

  ret.bytesProcessed = 0;

  // SSL_read_ex returned <=0. Determine why using SSL_get_error to decide whether this
  // indicates an orderly close (ZERO_RETURN), a retry condition (WANT_READ/WANT_WRITE),
  // or a transient/cryptic SYSCALL with errno==0 and no OpenSSL errors which should be
  // treated as non-fatal would-block.
  const auto err = ::SSL_get_error(_ssl.get(), 0);
  if (err == SSL_ERROR_ZERO_RETURN) {
    // Clean shutdown from the peer.
    return ret;
  }
  if (isRetry(err)) {
    ret.want = (err == SSL_ERROR_WANT_WRITE) ? TransportHint::WriteReady : TransportHint::ReadReady;
    return ret;
  }

  // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
  if (err == SSL_ERROR_SYSCALL) {
    if (errno == EAGAIN) {
      ret.want = TransportHint::ReadReady;
      return ret;
    }
    // Some platforms may present SSL_ERROR_SYSCALL with errno==0 and no OpenSSL errors
    // during non-blocking handshakes; treat this as a non-fatal would-block to avoid
    // prematurely closing the connection on transient EOF readings.
    if (errno == 0 && ::ERR_peek_error() == 0) {
      ret.want = TransportHint::ReadReady;
      return ret;
    }
  }
  ret.want = TransportHint::Error;
  return ret;
}

ITransport::TransportResult TlsTransport::write(std::string_view data) {
  TransportResult ret{0, handshake(TransportHint::WriteReady)};
  if (ret.want != TransportHint::None) {
    // indicate would-block during handshake
    return ret;
  }

  // Avoid calling OpenSSL with a zero-length buffer. Some OpenSSL builds
  // treat a null/zero-length pointer as an invalid argument and return
  // 'bad length'. If there's nothing to write, simply return 0.
  if (data.empty() || ::SSL_write_ex(_ssl.get(), data.data(), data.size(), &ret.bytesProcessed) == 1) {
    return ret;
  }

  const auto err = ::SSL_get_error(_ssl.get(), 0);
  if (isRetry(err)) {
    ret.want = (err == SSL_ERROR_WANT_WRITE) ? TransportHint::WriteReady : TransportHint::ReadReady;
    ret.bytesProcessed = 0;  // return 0 so caller retries with same data!
    return ret;
  }

  // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
  if (err == SSL_ERROR_SYSCALL) {
    auto saved_errno = errno;
    if (saved_errno == EAGAIN || (saved_errno == 0 && ERR_peek_error() == 0)) {
      ret.want = TransportHint::WriteReady;
      ret.bytesProcessed = 0;
      return ret;
    }
  }

  logErrorIfAny();

  ret.want = TransportHint::Error;
  ret.bytesProcessed = 0;

  return ret;
}

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

void TlsTransport::logErrorIfAny() const noexcept {
  for (auto errVal = ::ERR_get_error(); errVal != 0; errVal = ::ERR_get_error()) {
    char errBuf[256];
    ::ERR_error_string_n(errVal, errBuf, sizeof(errBuf));
    log::error("TLS transport OpenSSL error: {} (handshake done={})", std::string_view(errBuf), _handshakeDone);
  }
}

TransportHint TlsTransport::handshake(TransportHint want) {
  if (!_handshakeDone) {
    const int handshakeRet = ::SSL_do_handshake(_ssl.get());
    if (handshakeRet == 1) {
      _handshakeDone = true;
    } else {
      const int err = ::SSL_get_error(_ssl.get(), handshakeRet);
      if (isRetry(err)) {
        return (err == SSL_ERROR_WANT_WRITE) ? TransportHint::WriteReady : TransportHint::ReadReady;
      }
      // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
      if (err == SSL_ERROR_SYSCALL && errno == EAGAIN) {
        return want;
      }
      return TransportHint::Error;
    }
  }
  return TransportHint::None;
}

KtlsEnableResult TlsTransport::enableKtlsSend() {
  if (_ktlsResult != KtlsEnableResult::Unknown) {
    return _ktlsResult;
  }
#ifdef BIO_CTRL_GET_KTLS_SEND
  _ktlsResult = KtlsEnableResult::Disabled;

  auto* wbio = ::SSL_get_wbio(_ssl.get());
  if (wbio == nullptr) [[unlikely]] {
    log::error("enableKtlsSend: writeBio == nullptr -> fail");
    return _ktlsResult;
  }

  const auto getRes = ::BIO_ctrl(wbio, BIO_CTRL_GET_KTLS_SEND, 0, nullptr);
  log::debug("enableKtlsSend: BIO_CTRL_GET_KTLS_SEND -> {}", getRes);
  if (getRes == 1) {
    _ktlsResult = KtlsEnableResult::Enabled;
  }
#else
  _ktlsResult = KtlsEnableResult::Unsupported;
#endif
  return _ktlsResult;
}

}  // namespace aeronet
