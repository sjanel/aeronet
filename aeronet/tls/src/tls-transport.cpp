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
#include "aeronet/zerocopy.hpp"

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
  if (data.empty()) {
    return ret;
  }

  // When kTLS send is enabled and zerocopy is active, try to bypass SSL_write.
  // The kernel handles encryption directly on the socket, allowing us to use
  // MSG_ZEROCOPY for large payloads (DMA from user pages to NIC).
  // If zerocopy write fails with an unsupported operation (EOPNOTSUPP), we
  // disable zerocopy and fall back to SSL_write for this and future calls.
  if (_zerocopyState.enabled() && data.size() >= kZeroCopyMinPayloadSize) {
    ret = writeZerocopy(data);
    if (ret.want != TransportHint::None || ret.bytesProcessed > 0) {
      return ret;
    }
  }

  // Standard SSL_write path (user-space encryption or kTLS without zerocopy).
  if (::SSL_write_ex(_ssl.get(), data.data(), data.size(), &ret.bytesProcessed) == 1) {
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
    const auto saved_errno = errno;
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

bool TlsTransport::enableZerocopy() noexcept {
  const auto result = EnableZeroCopy(_fd);
  _zerocopyState.setEnabled(result == ZeroCopyEnableResult::Enabled);
  return _zerocopyState.enabled();
}

ITransport::TransportResult TlsTransport::writeZerocopy(std::string_view data) {
  TransportResult ret{0, TransportHint::None};

  // Drain pending completion notifications before issuing a new zerocopy send.
  // This prevents the kernel error queue from growing unbounded, avoids ENOBUFS,
  // and releases pinned pages promptly — critical for virtual devices (veth in K8s).
  pollZerocopyCompletions();

  // Use zerocopy sendmsg for large payloads when kTLS is active.
  // The kernel handles encryption, so we can DMA directly from user pages.
  const auto nbWritten = ZerocopySend(_fd, data, _zerocopyState);
  if (nbWritten >= 0) {
    ret.bytesProcessed = static_cast<std::size_t>(nbWritten);
    return ret;
  }
  static_assert(EOPNOTSUPP == ENOTSUP);
  if (errno == EOPNOTSUPP) {
    log::debug("MSG_ZEROCOPY not supported on kTLS socket fd # {}", _fd);
    // Disable zerocopy for this transport and fall through to SSL_write
    disableZerocopy();
  } else if (errno == EINTR) {
    // Fall through to regular send
  } else if (errno == EAGAIN) {
    ret.want = TransportHint::WriteReady;
  } else if (errno == ENOBUFS) {
    // Kernel cannot pin more pages for zerocopy — fall through to SSL_write path.
    // This is a transient condition, not a fatal error.
  } else {
    ret.want = TransportHint::Error;
  }

  return ret;
}

}  // namespace aeronet
