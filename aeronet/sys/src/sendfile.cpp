#include "aeronet/sendfile.hpp"

#include <cstddef>
#include <cstdint>

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

#include "aeronet/native-handle.hpp"

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
  const HANDLE fileHandle = reinterpret_cast<HANDLE>(_get_osfhandle(fileFd));
  if (fileHandle == INVALID_HANDLE_VALUE) {
    return -1;
  }
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
  ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == nullptr) {
    return -1;
  }
  const DWORD toSend = static_cast<DWORD>(count);
  if (::TransmitFile(outFd, fileHandle, toSend, 0, &ov, nullptr, TF_USE_DEFAULT_WORKER)) {
    // Completed synchronously.
    CloseHandle(ov.hEvent);
    offset += static_cast<int64_t>(toSend);
    return static_cast<int64_t>(toSend);
  }
  const DWORD err = WSAGetLastError();
  if (err == WSA_IO_PENDING) {
    // Socket is overlapped/non-blocking — wait for the operation to complete.
    DWORD bytesTransferred = 0;
    DWORD flags = 0;
    if (!WSAGetOverlappedResult(outFd, &ov, &bytesTransferred, TRUE, &flags)) {
      CloseHandle(ov.hEvent);
      return -1;
    }
    CloseHandle(ov.hEvent);
    offset += static_cast<int64_t>(bytesTransferred);
    return static_cast<int64_t>(bytesTransferred);
  }
  // Other errors (WSAEWOULDBLOCK, etc.) — propagate to caller.
  CloseHandle(ov.hEvent);
  WSASetLastError(err);
  return -1;
}
#endif

}  // namespace aeronet
