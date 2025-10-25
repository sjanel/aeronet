#include "transport.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string_view>

namespace aeronet {

ITransport::TransportResult PlainTransport::read(char* buf, std::size_t len) {
  const auto nbRead = ::read(_fd, buf, len);
  TransportResult ret{static_cast<std::size_t>(nbRead), TransportHint::None};
  if (nbRead >= 0) {
    return ret;
  }
  // ret == -1
  ret.bytesProcessed = 0;
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    ret.want = TransportHint::ReadReady;
    return ret;
  }
  ret.want = TransportHint::Error;
  return ret;
}

ITransport::TransportResult PlainTransport::write(std::string_view data) {
  TransportResult ret{0, TransportHint::None};

  while (ret.bytesProcessed < data.size()) {
    const auto res = ::write(_fd, data.data() + ret.bytesProcessed, data.size() - ret.bytesProcessed);

    if (res > 0) {
      ret.bytesProcessed += static_cast<std::size_t>(res);
    } else if (res == -1 && errno == EINTR) {
      // Interrupted by signal, retry immediately
      continue;
    } else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Kernel send buffer full — caller should wait for writable event
      ret.want = TransportHint::WriteReady;
      break;
    } else {
      // Fatal error (ECONNRESET, EPIPE, etc.)
      ret.want = TransportHint::Error;
      break;
    }
  }

  return ret;
}

}  // namespace aeronet