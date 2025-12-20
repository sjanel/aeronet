#include "aeronet/transport.hpp"

#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <string_view>

namespace aeronet {

static_assert(EAGAIN == EWOULDBLOCK, "Add handling for EWOULDBLOCK if different from EAGAIN");

ITransport::TransportResult PlainTransport::read(char* buf, std::size_t len) {
  const auto nbRead = ::read(_fd, buf, len);
  TransportResult ret{static_cast<std::size_t>(nbRead), TransportHint::None};
  if (nbRead == -1) [[unlikely]] {
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

  while (ret.bytesProcessed < data.size()) {
    const auto nbWritten = ::write(_fd, data.data() + ret.bytesProcessed, data.size() - ret.bytesProcessed);
    if (nbWritten == -1) [[unlikely]] {
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
  std::array<iovec, 2> iov{{// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                            {const_cast<char*>(firstBuf.data()), firstBuf.size()},
                            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                            {const_cast<char*>(secondBuf.data()), secondBuf.size()}}};

  TransportResult ret{0, TransportHint::None};
  const std::size_t totalSize = firstBuf.size() + secondBuf.size();

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

    const auto nbWritten = ::writev(_fd, iov.data() + iovIdx, static_cast<int>(iov.size()) - iovIdx);
    if (nbWritten == -1) [[unlikely]] {
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