#include "aeronet/sendfile.hpp"

#include <sys/types.h>

#include <cstddef>
#include <cstdint>

#ifdef __linux__
#include <sys/sendfile.h>
#elifdef __APPLE__
#include <sys/socket.h>
#include <sys/uio.h>
#endif

namespace aeronet {

int64_t Sendfile(int outFd, int inFd, off_t& offset, std::size_t count) noexcept {
#ifdef __linux__
  static_assert(sizeof(ssize_t) <= sizeof(int64_t), "ssize_t must fit in int64_t");
  return static_cast<int64_t>(::sendfile(outFd, inFd, &offset, count));
#elifdef __APPLE__
  auto len = static_cast<off_t>(count);
  const int rc = ::sendfile(inFd, outFd, offset, &len, nullptr, 0);
  if (rc == -1 && len == 0) {
    return -1;
  }
  offset += len;
  return static_cast<int64_t>(len);
#else
#error "Sendfile not implemented for this platform"
#endif
}

}  // namespace aeronet
