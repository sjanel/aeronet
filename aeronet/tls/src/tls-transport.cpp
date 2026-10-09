#include "aeronet/tls-transport.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/ssl3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/tls-ktls.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/zerocopy.hpp"

namespace aeronet {

namespace {
constexpr bool isRetry(int code) { return code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE; }

// Largest plaintext of a TLS record: what a single SSL_write_ex call encrypts at most (with partial writes).
constexpr std::size_t kMaxRecordPlainSize = SSL3_RT_MAX_PLAIN_LENGTH;
}  // namespace

TlsTransport::TlsTransport(SslPtr sslPtr, uint32_t minBytesForZerocopy)
    : _ssl(std::move(sslPtr)),
      _minBytesForZerocopy(minBytesForZerocopy),
      _readUntilDrained(_ssl && ::SSL_get_read_ahead(_ssl.get()) != 0) {
  if (_ssl) {
    ::SSL_set_mode(_ssl.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  }
}

bool TlsTransport::hasPendingReadData() const noexcept {
  switch (_readStop) {
    case ReadStop::Drained:
      return false;
    case ReadStop::Pending:
      [[fallthrough]];
    case ReadStop::Failed:
      return true;
    default:
      return _handshakeDone && _ssl && (::SSL_has_pending(_ssl.get()) != 0);
  }
}

TransportResult TlsTransport::read(char* buf, std::size_t len) {
  if (_readStop == ReadStop::Failed) [[unlikely]] {
    return {0, TransportHint::Error};
  }
  _readStop = ReadStop::None;

  TransportResult ret{0, handshake(TransportHint::ReadReady)};
  if (ret.want != TransportHint::None) {
    return ret;  // indicate would-block during handshake
  }

  if (::SSL_read_ex(_ssl.get(), buf, len, &ret.bytesProcessed) != 1) [[unlikely]] {
    ret.bytesProcessed = 0;
    ret.want = readFailureHint();
    if (_readUntilDrained && ret.want == TransportHint::ReadReady) {
      _readStop = ReadStop::Drained;
    }
    return ret;
  }

  if (_readUntilDrained) {
    // Read-ahead buffered the following records (as much as the socket had): decrypting them costs no syscall, and
    // handing them in one go to the caller lets it process the input in larger batches.
    std::size_t nbRead;
    while (ret.bytesProcessed < len) {
      if (::SSL_read_ex(_ssl.get(), buf + ret.bytesProcessed, len - ret.bytesProcessed, &nbRead) != 1) {
        // Return the data read so far. What stopped the reads is reported by the next one, unless the socket would
        // block. A fatal error is logged now, to leave the per-thread OpenSSL error queue empty.
        switch (readFailureHint()) {
          case TransportHint::ReadReady:
            _readStop = ReadStop::Drained;
            break;
          case TransportHint::Error:
            _readStop = ReadStop::Failed;
            break;
          default:
            _readStop = ReadStop::Pending;
            break;
        }
        break;
      }
      ret.bytesProcessed += nbRead;
    }
  }
  return ret;
}

TransportHint TlsTransport::readFailureHint() const {
  // SSL_read_ex failed. Determine why using SSL_get_error to decide whether this
  // indicates an orderly close (ZERO_RETURN), a retry condition (WANT_READ/WANT_WRITE),
  // or a transient/cryptic SYSCALL with err==0 and no OpenSSL errors which should be
  // treated as non-fatal would-block.
  const auto err = ::SSL_get_error(_ssl.get(), 0);
  if (err == SSL_ERROR_ZERO_RETURN) {
    // Clean shutdown from the peer.
    return TransportHint::None;
  }
  if (isRetry(err)) {
    return (err == SSL_ERROR_WANT_WRITE) ? TransportHint::WriteReady : TransportHint::ReadReady;
  }

  // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK should be treated as retry
  if (err == SSL_ERROR_SYSCALL) {
    const int sysErr = LastSystemError();
    if (sysErr == error::kWouldBlock) {
      return TransportHint::ReadReady;
    }
    // Some platforms may present SSL_ERROR_SYSCALL with err==0 and no OpenSSL errors
    // during non-blocking handshakes; treat this as a non-fatal would-block to avoid
    // prematurely closing the connection on transient EOF readings.
    if (sysErr == 0 && ::ERR_peek_error() == 0) {
      return TransportHint::ReadReady;
    }
  }
  // Fatal error: SSL_get_error() reached here only because ERR_peek_error() was non-empty (or
  // a real SYSCALL errno). Drain and log the queued error so it can never leak onto the shared
  // per-thread OpenSSL error queue, where it would poison SSL_get_error() for the next I/O op
  // on any connection of this event-loop thread. (write() already does this; keep read/write
  // symmetric so every non-retryable path leaves the queue empty.)
  logErrorIfAny();
  return TransportHint::Error;
}

TransportResult TlsTransport::write(std::string_view data) {
  if (isKtlsSendEnabled()) {
    // The kernel encrypts: write the data in clear on the socket, with MSG_ZEROCOPY for large payloads when enabled.
    return socketWrite(data);
  }

  TransportResult ret{0, handshake(TransportHint::WriteReady)};
  if (ret.want != TransportHint::None) {
    // indicate would-block during handshake
    return ret;
  }

  // Avoid calling OpenSSL with a zero-length buffer. Some OpenSSL builds treat a null/zero-length pointer as an invalid
  // argument and return 'bad length'. If there's nothing to write, simply return 0.
  if (data.empty()) {
    return ret;
  }

  return sslWrite(data);
}

TransportResult TlsTransport::write(std::string_view firstBuf, std::string_view secondBuf) {
  if (isKtlsSendEnabled()) {
    return socketWrite(firstBuf, secondBuf);
  }
  const std::string_view buffers[]{firstBuf, secondBuf};
  return write(std::span<const std::string_view>(buffers));
}

TransportResult TlsTransport::write(std::span<const std::string_view> buffers) {
  if (isKtlsSendEnabled()) {
    // The kernel builds full records across the buffers: one gather write, without any copy.
    return socketWrite(buffers);
  }

  TransportResult result{0, handshake(TransportHint::WriteReady)};
  if (result.want != TransportHint::None) {
    return result;
  }

  // OpenSSL encrypts the data of each SSL_write_ex call into its own records. A buffer that fills whole records, or the
  // last one, is encrypted in place. A smaller one is copied with the start of the following ones into a full record.
  // Retrying a write that would block rebuilds the same record from the same pending data (OpenSSL accepts its moving
  // buffer, as long as it starts with the same bytes).
  char record[kMaxRecordPlainSize];
  auto bufIt = buffers.begin();
  std::size_t bufOffset = 0;  // already written bytes of *bufIt
  while (bufIt != buffers.end()) {
    const std::string_view head = bufIt->substr(bufOffset);
    if (head.empty()) {
      ++bufIt;
      bufOffset = 0;
      continue;
    }
    std::string_view chunk = head;
    if (head.size() < kMaxRecordPlainSize && bufIt + 1 != buffers.end()) {
      char* out = Append(head.data(), head.size(), record);
      for (auto nextIt = bufIt + 1; nextIt != buffers.end() && out != record + kMaxRecordPlainSize; ++nextIt) {
        const std::string_view part = nextIt->substr(0, static_cast<std::size_t>(record + kMaxRecordPlainSize - out));
        out = Append(part.data(), part.size(), out);
      }
      chunk = std::string_view(record, static_cast<std::size_t>(out - record));
    }

    const auto [nbWritten, want] = sslWrite(chunk);
    result.bytesProcessed += nbWritten;
    result.want = want;
    if (want != TransportHint::None) {
      break;
    }
    // Partial writes: OpenSSL returns once a record is written. Move forward the written bytes.
    for (std::size_t remaining = nbWritten; remaining != 0;) {
      const std::size_t consumed = std::min(remaining, bufIt->size() - bufOffset);
      remaining -= consumed;
      bufOffset += consumed;
      if (bufOffset == bufIt->size()) {
        ++bufIt;
        bufOffset = 0;
      }
    }
  }
  return result;
}

TransportResult TlsTransport::sslWrite(std::string_view data) {
  TransportResult ret{0, TransportHint::None};
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
    const auto savedErr = LastSystemError();
    if (savedErr == error::kWouldBlock || (savedErr == 0 && ERR_peek_error() == 0)) {
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
  if (!_handshakeDone) {
    // Connection closed before TLS handshake completed (e.g., plain TCP probe).
    // SSL_shutdown on a non-handshaked SSL is undefined — just clear stale errors.
    ::ERR_clear_error();
    return;
  }
  // Best-effort graceful close only. A second immediate SSL_shutdown() call can
  // re-enter OpenSSL's shutdown state machine while the connection is already
  // being force-closed by the server, which has shown up as rare stop-time
  // stalls on Windows after large TLS transfers. One call is enough to emit a
  // close_notify when possible; the outer layer will close the socket either way.
  (void)::SSL_shutdown(ssl);
  // Clear any errors left on the per-thread OpenSSL error queue by the shutdown.
  // Without this, stale errors can pollute subsequent SSL_read_ex/SSL_write_ex calls
  // on other SSL connections sharing the same thread (the error queue is per-thread,
  // not per-SSL object), causing ERR_peek_error() checks to misclassify transient
  // conditions as fatal errors.
  ::ERR_clear_error();
}

void TlsTransport::logErrorIfAny() const {
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
      // SSL_ERROR_SYSCALL with EAGAIN/EWOULDBLOCK or sysErr==0 (spurious wakeup) should be treated as retry
      if (err == SSL_ERROR_SYSCALL) {
        const int sysErr = LastSystemError();
        if (sysErr == error::kWouldBlock || (sysErr == 0 && ::ERR_peek_error() == 0)) {
          return want;
        }
      }
      // Fatal handshake failure (e.g. a plaintext probe against the TLS port hangs up, leaving
      // "unexpected eof while reading" queued). Drain and log it here so the error cannot linger
      // on the shared per-thread queue and corrupt SSL_get_error() for a later I/O op.
      logErrorIfAny();
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
    // Application data is now written directly on the socket (see write()).
    _fd = static_cast<NativeHandle>(::SSL_get_wfd(_ssl.get()));
    _ktlsResult = KtlsEnableResult::Enabled;
  }
#else
  _ktlsResult = KtlsEnableResult::Unsupported;
#endif
  return _ktlsResult;
}

bool TlsTransport::enableZerocopy() noexcept {
  if (EnableZeroCopy(_fd) == ZeroCopyEnableResult::Enabled) {
    _zerocopyState.enable(_minBytesForZerocopy);
  } else {
    _zerocopyState.disable();
  }
  return _zerocopyState.enabled();
}

}  // namespace aeronet
