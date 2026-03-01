#include "aeronet/transport.hpp"

#include "aeronet/platform.hpp"

#ifdef AERONET_POSIX
#include <sys/uio.h>  // NOLINT(misc-include-cleaner) used by iovec
#include <unistd.h>
#elifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#endif

#include <cstddef>
#include <string_view>

#include "aeronet/log.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "aeronet/zerocopy.hpp"

namespace aeronet {

#ifdef AERONET_POSIX
static_assert(EAGAIN == EWOULDBLOCK, "Add handling for EWOULDBLOCK if different from EAGAIN");
#endif

PlainTransport::PlainTransport(NativeHandle fd, ZerocopyMode zerocopyMode, uint32_t minBytesForZerocopy)
    : ITransport(fd, minBytesForZerocopy) {
  if (zerocopyMode != ZerocopyMode::Disabled) {
    const auto result = EnableZeroCopy(_fd);
    _zerocopyState.setEnabled(result == ZeroCopyEnableResult::Enabled);
    if (!_zerocopyState.enabled() && zerocopyMode == ZerocopyMode::Enabled) {
      log::warn("Failed to enable MSG_ZEROCOPY on fd # {}", fd);
    }
  }
}

ITransport::TransportResult PlainTransport::read(char* buf, std::size_t len) {
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

ITransport::TransportResult PlainTransport::write(std::string_view data) {
  TransportResult ret{0, TransportHint::None};

  // Try zerocopy for large payloads if enabled
  if (_zerocopyState.enabled() && data.size() >= _minBytesForZerocopy) {
    // Drain pending completion notifications before issuing a new zerocopy send.
    // This prevents the kernel error queue from growing unbounded, avoids ENOBUFS,
    // and releases pinned pages promptly — critical for virtual devices (veth in K8s).
    pollZerocopyCompletions();
    const auto nbWritten = ZerocopySend(_fd, data, _zerocopyState);
    if (nbWritten >= 0) {
      ret.bytesProcessed = static_cast<std::size_t>(nbWritten);
      return ret;
    }
    const int zcErr = LastSystemError();
    if (zcErr == error::kWouldBlock) {
      ret.want = TransportHint::WriteReady;
      return ret;
    }
    // On error, check if retryable
    if (zcErr == error::kInterrupted) {  // NOLINT(bugprone-branch-clone)
      // Fall through to regular write loop
    } else if (zcErr == error::kNoBufferSpace) {
      // Kernel cannot pin more pages for zerocopy — fall through to regular write path.
      // This is a transient condition, not a fatal error.
    } else {
      ret.want = TransportHint::Error;
      return ret;
    }
  }

  // Regular write path (fallback or small payloads)
  // Note: Using write() for compatibility with existing test infrastructure.
  // SIGPIPE is handled at the error level (EPIPE).
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
        // Fatal error (ECONNRESET, EPIPE, etc.)
        ret.want = TransportHint::Error;
      }
      break;
    }

    ret.bytesProcessed += static_cast<std::size_t>(nbWritten);
  }

  return ret;
}

ITransport::TransportResult PlainTransport::write(std::string_view firstBuf, std::string_view secondBuf) {
  // Use writev / WSASend for scatter-gather I/O - single syscall for both buffers.
  // This avoids extra memcpy and allows optimal TCP segmentation.
#ifdef AERONET_POSIX
  // NOLINTNEXTLINE(misc-include-cleaner)
  iovec iov[]{// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
              {const_cast<char*>(firstBuf.data()), firstBuf.size()},
              // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
              {const_cast<char*>(secondBuf.data()), secondBuf.size()}};
#elifdef AERONET_WINDOWS
  WSABUF iov[]{{static_cast<ULONG>(firstBuf.size()), const_cast<char*>(firstBuf.data())},
               {static_cast<ULONG>(secondBuf.size()), const_cast<char*>(secondBuf.data())}};
#endif

  TransportResult ret{0, TransportHint::None};
  const std::size_t totalSize = firstBuf.size() + secondBuf.size();

  // Try zerocopy for large payloads if enabled
  if (_zerocopyState.enabled() && totalSize >= _minBytesForZerocopy) {
    // Drain pending completion notifications before issuing a new zerocopy send.
    pollZerocopyCompletions();

    const auto nbWritten = ZerocopySend(_fd, firstBuf, secondBuf, _zerocopyState);
    if (nbWritten >= 0) {
      ret.bytesProcessed = static_cast<std::size_t>(nbWritten);
      return ret;
    }
    // On error, check if retryable
    const int zcErr = LastSystemError();
    if (zcErr == error::kInterrupted) {  // NOLINT(bugprone-branch-clone)
      // Fall through to regular write loop
    } else if (zcErr == error::kWouldBlock) {
      ret.want = TransportHint::WriteReady;
      return ret;
    } else if (zcErr == error::kNoBufferSpace) {
      // Kernel cannot pin more pages for zerocopy — fall through to regular write path.
      // This is a transient condition, not a fatal error.
    } else {
      ret.want = TransportHint::Error;
      return ret;
    }
  }

  while (ret.bytesProcessed < totalSize) {
    // Adjust iovec based on bytes already written
    int iovIdx = 0;
    std::size_t offset = ret.bytesProcessed;

    if (offset >= firstBuf.size()) {
      // First buffer fully written, start from second
      iovIdx = 1;
      offset -= firstBuf.size();
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
#ifdef AERONET_POSIX
      iov[1].iov_base = const_cast<char*>(secondBuf.data()) + offset;
      iov[1].iov_len = secondBuf.size() - offset;
#elifdef AERONET_WINDOWS
      iov[1].buf = const_cast<char*>(secondBuf.data()) + offset;
      iov[1].len = static_cast<ULONG>(secondBuf.size() - offset);
#endif
    } else {
      // Still writing first buffer
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
#ifdef AERONET_POSIX
      iov[0].iov_base = const_cast<char*>(firstBuf.data()) + offset;
      iov[0].iov_len = firstBuf.size() - offset;
#elifdef AERONET_WINDOWS
      iov[0].buf = const_cast<char*>(firstBuf.data()) + offset;
      iov[0].len = static_cast<ULONG>(firstBuf.size() - offset);
#endif
    }

#ifdef AERONET_POSIX
    const auto nbWritten = ::writev(_fd, iov + iovIdx, static_cast<int>(std::size(iov)) - iovIdx);
    if (nbWritten == -1) {
#elifdef AERONET_WINDOWS
    DWORD bytesSent = 0;
    const int wsaResult =
        ::WSASend(_fd, iov + iovIdx, static_cast<DWORD>(std::size(iov)) - iovIdx, &bytesSent, 0, nullptr, nullptr);
    const auto nbWritten = (wsaResult == 0) ? static_cast<ssize_t>(bytesSent) : static_cast<ssize_t>(-1);
    if (nbWritten == -1) {
#endif
      const int err = LastSystemError();
      if (err == error::kInterrupted) {
        continue;
      }
      if (err == error::kWouldBlock) {
        ret.want = TransportHint::WriteReady;
      } else {
        ret.want = TransportHint::Error;
      }
      break;
    }

    ret.bytesProcessed += static_cast<std::size_t>(nbWritten);
  }

  return ret;
}

}  // namespace aeronet