#include "transport.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string_view>

namespace aeronet {

std::size_t PlainTransport::read(char* buf, std::size_t len, TransportHint& want) {
  const auto ret = ::read(_fd, buf, len);
  if (ret >= 0) {
    want = TransportHint::None;
    return static_cast<std::size_t>(ret);
  }
  // ret == -1
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    want = TransportHint::ReadReady;
    return 0;
  }
  want = TransportHint::Error;
  return 0;
}

std::size_t PlainTransport::write(std::string_view data, TransportHint& want) {
  want = TransportHint::None;

  std::size_t total = 0;

  while (total < data.size()) {
    const auto res = ::write(_fd, data.data() + total, data.size() - total);

    if (res > 0) {
      total += static_cast<std::size_t>(res);
    } else if (res == -1 && errno == EINTR) {
      // Interrupted by signal, retry immediately
      continue;
    } else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Kernel send buffer full — caller should wait for writable event
      want = TransportHint::WriteReady;
      break;
    } else {
      // Fatal error (ECONNRESET, EPIPE, etc.)
      want = TransportHint::Error;
      break;
    }
  }

  // Return how much we actually wrote.
  // If total == 0 and wantWrite==true, treat it like EAGAIN/no progress.
  return total;
}

}  // namespace aeronet