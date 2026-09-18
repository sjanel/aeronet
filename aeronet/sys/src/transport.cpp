#include "aeronet/transport.hpp"

#ifdef AERONET_POSIX
#include <sys/uio.h>  // NOLINT(misc-include-cleaner) used by iovec
#include <unistd.h>
#elifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#endif

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/file.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/sendfile.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "aeronet/zerocopy.hpp"

namespace aeronet {

PlainTransport::PlainTransport(NativeHandle fd, ZerocopyMode zerocopyMode, uint32_t minBytesForZerocopy)
    : SocketTransportState(fd, minBytesForZerocopy) {
  if (zerocopyMode != ZerocopyMode::Disabled) {
    const auto result = EnableZeroCopy(_fd);
    _zerocopyState.setEnabled(result == ZeroCopyEnableResult::Enabled);
    if (!_zerocopyState.enabled() && zerocopyMode == ZerocopyMode::Enabled) {
      log::warn("Failed to enable MSG_ZEROCOPY on fd # {}", fd);
    }
  }
}

TransportResult PlainTransport::read(char* buf, std::size_t len) {
#ifdef AERONET_POSIX
  const auto nbRead = ::read(_fd, buf, len);
#elifdef AERONET_WINDOWS
  const auto nbRead = ::recv(_fd, buf, static_cast<int>(len), 0);
#endif
  TransportResult ret{static_cast<std::size_t>(nbRead), TransportHint::None};
  if (nbRead == -1) {
    ret.bytesProcessed = 0;

    const int err = LastSystemError();
    if (err == error::kInterrupted || err == error::kWouldBlock) {
      ret.want = TransportHint::ReadReady;
    } else {
      ret.want = TransportHint::Error;
    }
  }
  return ret;
}

TransportResult PlainTransport::write(std::string_view data) {
#ifdef AERONET_LINUX
  TransportResult ret = _zerocopyState.tryZerocopySend(_fd, _minBytesForZerocopy, data);
  if (ret.bytesProcessed != 0 || ret.want != TransportHint::None) {
    return ret;
  }
#else
  TransportResult ret{};
#endif

  // Regular write path (fallback or small payloads)
  // Note: Using write() for compatibility with existing test infrastructure.
  // SIGPIPE is handled at the error level (error::kBrokenPipe).
  while (ret.bytesProcessed < data.size()) {
#ifdef AERONET_POSIX
    const auto nbWritten = ::write(_fd, data.data() + ret.bytesProcessed, data.size() - ret.bytesProcessed);
#elifdef AERONET_WINDOWS
    const auto nbWritten =
        ::send(_fd, data.data() + ret.bytesProcessed, static_cast<int>(data.size() - ret.bytesProcessed), 0);
#endif
    if (nbWritten == -1) {
      const int err = LastSystemError();
      if (err == error::kInterrupted) {
        // Interrupted by signal, retry immediately
        continue;
      }
      if (err == error::kWouldBlock) {
        // Kernel send buffer full — caller should wait for writable event
        ret.want = TransportHint::WriteReady;
      } else {
        // Fatal error (error::kConnectionReset, error::kBrokenPipe, etc.)
        ret.want = TransportHint::Error;
      }
      break;
    }

    ret.bytesProcessed += static_cast<std::size_t>(nbWritten);
  }

  return ret;
}

TransportResult PlainTransport::write(std::string_view firstBuf, std::string_view secondBuf) {
#ifdef AERONET_LINUX
  TransportResult ret = _zerocopyState.tryZerocopySend(_fd, _minBytesForZerocopy, firstBuf, secondBuf);
  if (ret.bytesProcessed != 0 || ret.want != TransportHint::None) {
    return ret;
  }
#endif
  const std::string_view buffers[]{firstBuf, secondBuf};
  return write(std::span<const std::string_view>(buffers));
}

TransportResult PlainTransport::write(std::span<const std::string_view> buffers) {
  static constexpr uint8_t kMaxGatherBuffers = 64;

#ifdef AERONET_POSIX
  iovec ioVectors[kMaxGatherBuffers];  // NOLINT(misc-include-cleaner)
#elifdef AERONET_WINDOWS
  WSABUF ioVectors[kMaxGatherBuffers];
#endif

  TransportResult result{0, TransportHint::None};
  std::size_t bufIdx = 0;  // next buffer to consider

  // This path intentionally uses kernel-copy scatter I/O. HTTP/2 releases accepted
  // fragments immediately, which is earlier than a MSG_ZEROCOPY completion.
  while (bufIdx < buffers.size()) {
    // Build a batch of at most kMaxGatherBuffers non empty iovecs.
    std::size_t ioVectorCount = 0;
    for (; bufIdx < buffers.size() && ioVectorCount < kMaxGatherBuffers; ++bufIdx) {
      const std::string_view buffer = buffers[bufIdx];
      if (buffer.empty()) {
        continue;
      }
#ifdef AERONET_POSIX
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      ioVectors[ioVectorCount] = {const_cast<char*>(buffer.data()), buffer.size()};
#elifdef AERONET_WINDOWS
      ioVectors[ioVectorCount] = {static_cast<ULONG>(buffer.size()), const_cast<char*>(buffer.data())};
#endif
      ++ioVectorCount;
    }

    std::size_t ioVectorIndex = 0;
    while (ioVectorIndex < ioVectorCount) {
#ifdef AERONET_POSIX
      const auto nbWritten = ::writev(_fd, ioVectors + ioVectorIndex, static_cast<int>(ioVectorCount - ioVectorIndex));
#elifdef AERONET_WINDOWS
      DWORD bytesSent = 0;
      const int wsaResult = ::WSASend(_fd, ioVectors + ioVectorIndex, static_cast<DWORD>(ioVectorCount - ioVectorIndex),
                                      &bytesSent, 0, nullptr, nullptr);
      const auto nbWritten = wsaResult == 0 ? static_cast<int64_t>(bytesSent) : static_cast<int64_t>(-1);
#endif
      if (nbWritten == -1) {
        const int err = LastSystemError();
        if (err == error::kInterrupted) {
          continue;
        }
        result.want = err == error::kWouldBlock ? TransportHint::WriteReady : TransportHint::Error;
        return result;  // we cannot continue to the next batch in case of error.
      }
      if (nbWritten == 0) [[unlikely]] {
        return result;
      }

      std::size_t remaining = static_cast<std::size_t>(nbWritten);
      result.bytesProcessed += remaining;
      while (ioVectorIndex < ioVectorCount) {
#ifdef AERONET_POSIX
        const std::size_t ioVectorSize = ioVectors[ioVectorIndex].iov_len;
#elifdef AERONET_WINDOWS
        const std::size_t ioVectorSize = ioVectors[ioVectorIndex].len;
#endif
        if (remaining < ioVectorSize) {
#ifdef AERONET_POSIX
          ioVectors[ioVectorIndex].iov_base = static_cast<char*>(ioVectors[ioVectorIndex].iov_base) + remaining;
          ioVectors[ioVectorIndex].iov_len -= remaining;
#elifdef AERONET_WINDOWS
          ioVectors[ioVectorIndex].buf += remaining;
          ioVectors[ioVectorIndex].len -= static_cast<ULONG>(remaining);
#endif
          break;
        }
        remaining -= ioVectorSize;
        ++ioVectorIndex;
      }
    }
    // Batch completely written. We can proceed to next buffers, if any.
  }

  return result;
}

TransportResult PlainTransport::sendFile(const File& file, std::size_t& offset, std::size_t count) {
  TransportResult ret{0, TransportHint::None};
  for (;;) {
    // Sendfile advances `offset` by the number of bytes sent (0 on error / would-block).
    const int64_t nbSent = Sendfile(_fd, static_cast<int>(file.fd()), offset, count);
    if (nbSent > 0) {
      ret.bytesProcessed = static_cast<std::size_t>(nbSent);
      break;
    }
    if (nbSent == 0) {
      // sendfile() returns 0 at end-of-input: the file was truncated below the region we promised to send
      // (its length was validated against the file size when the payload was set). The declared
      // Content-Length can no longer be honored, so surface a fatal error rather than spin.
      ret.want = TransportHint::Error;
      break;
    }
    const int err = LastSystemError();
    if (err == error::kInterrupted) {
      continue;  // interrupted before any byte was sent: retry immediately
    }
    // Kernel send buffer full => wait for writable; anything else is fatal (peer reset, broken pipe, ...).
    ret.want = (err == error::kWouldBlock) ? TransportHint::WriteReady : TransportHint::Error;
    break;
  }
  return ret;
}

TransportKind Transport::kind() const noexcept {
  if (_storage.index() == kEmptyIndex) {
    return TransportKind::Empty;
  }
  if (_storage.index() == kPlainIndex) {
    return TransportKind::Plain;
  }
  return operations().kind;
}

}  // namespace aeronet
