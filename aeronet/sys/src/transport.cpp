#include "aeronet/transport.hpp"

#include <unistd.h>

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

}  // namespace aeronet