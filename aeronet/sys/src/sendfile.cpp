#include "aeronet/sendfile.hpp"

#include <cstddef>
#include <cstdint>

#include "aeronet/platform.hpp"

#ifdef AERONET_LINUX
#include <sys/sendfile.h>
#include <sys/types.h>
#elifdef AERONET_MACOS
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#elifdef AERONET_WINDOWS
#include <io.h>  // _get_osfhandle
#include <mswsock.h>
#endif

namespace aeronet {

#ifdef AERONET_POSIX
int64_t Sendfile(NativeHandle outFd, NativeHandle inFd, off_t& offset, std::size_t count) noexcept {
#ifdef AERONET_LINUX
  static_assert(sizeof(ssize_t) <= sizeof(int64_t), "ssize_t must fit in int64_t");
  return static_cast<int64_t>(::sendfile(outFd, inFd, &offset, count));
#elifdef AERONET_MACOS
  auto len = static_cast<off_t>(count);
  const int rc = ::sendfile(inFd, outFd, offset, &len, nullptr, 0);
  if (rc == -1 && len == 0) {
    return -1;
  }
  offset += len;
  return static_cast<int64_t>(len);
#endif
}

#elifdef AERONET_WINDOWS
int64_t Sendfile(NativeHandle outFd, int fileFd, int64_t& offset, std::size_t count) noexcept {
  // Convert CRT file descriptor to a Win32 HANDLE for TransmitFile.
  const HANDLE fileHandle = reinterpret_cast<HANDLE>(::_get_osfhandle(fileFd));
  if (fileHandle == INVALID_HANDLE_VALUE) {
    return -1;
  }
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
  const DWORD toSend = static_cast<DWORD>(count);
  if (!::TransmitFile(outFd, fileHandle, toSend, 0, &ov, nullptr, TF_USE_DEFAULT_WORKER)) {
    return -1;
  }
  // TransmitFile is synchronous when called on a blocking socket or with an
  // OVERLAPPED on a non-overlapped socket. On success the full count was sent.
  offset += static_cast<int64_t>(toSend);
  return static_cast<int64_t>(toSend);
}
#endif

}  // namespace aeronet
