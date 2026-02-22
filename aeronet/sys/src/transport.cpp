#include "aeronet/transport.hpp"

#include <sys/uio.h>  // NOLINT(misc-include-cleaner) used by iovec
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string_view>

#include "aeronet/log.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "aeronet/zerocopy.hpp"

namespace aeronet {

static_assert(EAGAIN == EWOULDBLOCK, "Add handling for EWOULDBLOCK if different from EAGAIN");

PlainTransport::PlainTransport(int fd, ZerocopyMode zerocopyMode, bool isZerocopyEnabled)
    : ITransport(fd), _forcedZerocopy(zerocopyMode == ZerocopyMode::Forced) {
  if (isZerocopyEnabled && zerocopyMode != ZerocopyMode::Disabled) {
    const auto result = EnableZeroCopy(_fd);
    _zerocopyState.setEnabled(result == ZeroCopyEnableResult::Enabled);
    if (!_zerocopyState.enabled() && (zerocopyMode == ZerocopyMode::Enabled || zerocopyMode == ZerocopyMode::Forced)) {
      log::warn("Failed to enable MSG_ZEROCOPY on fd # {}", fd);
    }
  }
}

ITransport::TransportResult PlainTransport::read(char* buf, std::size_t len) {
  const auto nbRead = ::read(_fd, buf, len);
  TransportResult ret{static_cast<std::size_t>(nbRead), TransportHint::None};
  if (nbRead == -1) {
    ret.bytesProcessed = 0;

    if (errno == EINTR || errno == EAGAIN) {
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
  if (_zerocopyState.enabled() && (_forcedZerocopy || data.size() >= kZeroCopyMinPayloadSize)) {
    // Drain pending completion notifications before issuing a new zerocopy send.
    // This prevents the kernel error queue from growing unbounded, avoids ENOBUFS,
    // and releases pinned pages promptly — critical for virtual devices (veth in K8s).
    pollZerocopyCompletions();
    const auto nbWritten = ZerocopySend(_fd, data, _zerocopyState);
    if (nbWritten >= 0) {
      ret.bytesProcessed = static_cast<std::size_t>(nbWritten);
      return ret;
    }
    if (errno == EAGAIN) {
      ret.want = TransportHint::WriteReady;
      return ret;
    }
    // On error, check if retryable
    if (errno == EINTR) {  // NOLINT(bugprone-branch-clone)
      // Fall through to regular write loop
    } else if (errno == ENOBUFS) {
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
    const auto nbWritten = ::write(_fd, data.data() + ret.bytesProcessed, data.size() - ret.bytesProcessed);
    if (nbWritten == -1) {
      if (errno == EINTR) {
        // Interrupted by signal, retry immediately
        continue;
      }
      if (errno == EAGAIN) {
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
  // Use writev for scatter-gather I/O - single syscall for both buffers.
  // This avoids extra memcpy and allows optimal TCP segmentation.
  // NOLINTNEXTLINE(misc-include-cleaner)
  iovec iov[]{// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
              {const_cast<char*>(firstBuf.data()), firstBuf.size()},
              // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
              {const_cast<char*>(secondBuf.data()), secondBuf.size()}};

  TransportResult ret{0, TransportHint::None};
  const std::size_t totalSize = firstBuf.size() + secondBuf.size();

  // Try zerocopy for large payloads if enabled
  if (_zerocopyState.enabled() && (_forcedZerocopy || totalSize >= kZeroCopyMinPayloadSize)) {
    // Drain pending completion notifications before issuing a new zerocopy send.
    pollZerocopyCompletions();

    const auto nbWritten = ZerocopySend(_fd, firstBuf, secondBuf, _zerocopyState);
    if (nbWritten >= 0) {
      ret.bytesProcessed = static_cast<std::size_t>(nbWritten);
      return ret;
    }
    // On error, check if retryable
    if (errno == EINTR) {  // NOLINT(bugprone-branch-clone)
      // Fall through to regular write loop
    } else if (errno == EAGAIN) {
      ret.want = TransportHint::WriteReady;
      return ret;
    } else if (errno == ENOBUFS) {
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
      iov[1].iov_base = const_cast<char*>(secondBuf.data()) + offset;
      iov[1].iov_len = secondBuf.size() - offset;
    } else {
      // Still writing first buffer
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      iov[0].iov_base = const_cast<char*>(firstBuf.data()) + offset;
      iov[0].iov_len = firstBuf.size() - offset;
    }

    const auto nbWritten = ::writev(_fd, iov + iovIdx, static_cast<int>(std::size(iov)) - iovIdx);
    if (nbWritten == -1) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN) {
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