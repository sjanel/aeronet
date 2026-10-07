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

#include <cassert>
#elifdef AERONET_WINDOWS
#include <io.h>  // _get_osfhandle
#include <mswsock.h>

#include <algorithm>
#include <string_view>

#include "aeronet/log.hpp"
#include "aeronet/system-error-message.hpp"
#endif

#include "aeronet/native-handle.hpp"

namespace aeronet {

#ifdef AERONET_WINDOWS
namespace {

// TransmitFile sends at most INT32_MAX - 1 bytes per call: a longer region is sent over several calls.
constexpr std::size_t kMaxTransmitFileBytes = 2147483646UL;

// Logs a failed step of Sendfile with its context, and leaves err as the last socket error for the caller.
int64_t SendfileFailure(std::string_view step, NativeHandle outFd, int fileFd, std::size_t offset, std::size_t count,
                        int err) {
  log::error("Sendfile: {} failed (socket # {}, file fd # {}, offset={}, count={}, err={}, msg={})", step, outFd,
             fileFd, offset, count, err, SystemErrorMessage(err));
  ::WSASetLastError(err);
  return -1;
}

}  // namespace
#endif

int64_t Sendfile(NativeHandle outFd, int fileFd, std::size_t& offset, std::size_t count) {
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

  const int rc = ::sendfile(fileFd, outFd, static_cast<off_t>(offset), &len, nullptr, 0);

  if (len > 0) {
    offset += static_cast<std::size_t>(len);
    return static_cast<int64_t>(len);
  }

  return (rc == 0) ? 0 : -1;
#elifdef AERONET_WINDOWS
  // Convert CRT file descriptor to a Win32 HANDLE for TransmitFile.
  const HANDLE fileHandle = reinterpret_cast<HANDLE>(_get_osfhandle(fileFd));
  if (fileHandle == INVALID_HANDLE_VALUE) {
    return SendfileFailure("_get_osfhandle", outFd, fileFd, offset, count, WSAEBADF);
  }
  // For a file opened for synchronous I/O (as _open() does), TransmitFile works from the current file position, which
  // the OVERLAPPED offset does not override: a transfer failed with WSAEINVAL once a previous one had left the position
  // at the end of the file. Position the file at the requested offset first.
  LARGE_INTEGER filePosition{};
  filePosition.QuadPart = static_cast<LONGLONG>(offset);
  if (!::SetFilePointerEx(fileHandle, filePosition, nullptr, FILE_BEGIN)) {
    return SendfileFailure("SetFilePointerEx", outFd, fileFd, offset, count, static_cast<int>(::GetLastError()));
  }
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
  ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == nullptr) {
    return SendfileFailure("CreateEvent", outFd, fileFd, offset, count, static_cast<int>(::GetLastError()));
  }
  const DWORD toSend = static_cast<DWORD>(std::min(count, kMaxTransmitFileBytes));
  if (::TransmitFile(outFd, fileHandle, toSend, 0, &ov, nullptr, TF_USE_DEFAULT_WORKER)) {
    // Completed synchronously.
    CloseHandle(ov.hEvent);
    offset += toSend;
    return static_cast<int64_t>(toSend);
  }
  // Read the error before CloseHandle(), which may overwrite it.
  const int err = WSAGetLastError();
  if (err == WSA_IO_PENDING) {
    // Socket is overlapped/non-blocking - wait for the operation to complete.
    DWORD bytesTransferred = 0;
    DWORD flags = 0;
    if (!WSAGetOverlappedResult(outFd, &ov, &bytesTransferred, TRUE, &flags)) {
      const int overlappedErr = WSAGetLastError();
      CloseHandle(ov.hEvent);
      return SendfileFailure("WSAGetOverlappedResult(TransmitFile)", outFd, fileFd, offset, count, overlappedErr);
    }
    CloseHandle(ov.hEvent);
    if (bytesTransferred == 0) [[unlikely]] {
      log::warn(
          "Sendfile: TransmitFile completed without sending any byte (socket # {}, file fd # {}, offset={}, "
          "count={})",
          outFd, fileFd, offset, count);
    }
    offset += bytesTransferred;
    return static_cast<int64_t>(bytesTransferred);
  }
  CloseHandle(ov.hEvent);
  if (err == WSAEWOULDBLOCK) {
    // Send buffer full: not an error, the caller waits for writability.
    WSASetLastError(err);
    return -1;
  }
  return SendfileFailure("TransmitFile", outFd, fileFd, offset, count, err);
#endif
}

}  // namespace aeronet
