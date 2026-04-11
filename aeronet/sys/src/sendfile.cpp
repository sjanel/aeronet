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

int64_t Sendfile(NativeHandle outFd, int fileFd, std::size_t& offset, std::size_t count) noexcept {
#ifdef AERONET_LINUX
  static_assert(sizeof(ssize_t) <= sizeof(int64_t), "ssize_t must fit in int64_t");
  off_t off = static_cast<off_t>(offset);
  const ssize_t result = ::sendfile(outFd, fileFd, &off, count);
  if (result > 0) {
    offset = static_cast<std::size_t>(off);
  }
  return static_cast<int64_t>(result);
#elifdef AERONET_MACOS
  auto len = static_cast<off_t>(count);
  const auto rc = ::sendfile(fileFd, outFd, static_cast<off_t>(offset), &len, nullptr, 0);
  if (rc == -1 && len == 0) {
    return -1;
  }
  offset += len;
  return static_cast<int64_t>(len);
#elifdef AERONET_WINDOWS
  // Convert CRT file descriptor to a Win32 HANDLE for TransmitFile.
  const HANDLE fileHandle = reinterpret_cast<HANDLE>(_get_osfhandle(fileFd));
  if (fileHandle == INVALID_HANDLE_VALUE) {
    return -1;
  }
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
  ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == nullptr) {
    return -1;
  }
  const DWORD toSend = static_cast<DWORD>(count);
  if (::TransmitFile(outFd, fileHandle, toSend, 0, &ov, nullptr, TF_USE_DEFAULT_WORKER)) {
    // Completed synchronously.
    CloseHandle(ov.hEvent);
    offset += toSend;
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
    offset += bytesTransferred;
    return static_cast<int64_t>(bytesTransferred);
  }
  // Other errors (WSAEWOULDBLOCK, etc.) — propagate to caller.
  CloseHandle(ov.hEvent);
  WSASetLastError(err);
  return -1;
#endif
}

}  // namespace aeronet
