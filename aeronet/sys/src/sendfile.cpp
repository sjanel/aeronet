#include "aeronet/sendfile.hpp"

#include <cstddef>
#include <cstdint>

#ifdef AERONET_IO_URING
#include <liburing.h>

#include <cerrno>
#endif
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

#ifdef AERONET_IO_URING
int64_t IoUringSpliceFile(void* ioRing, NativeHandle pipeRead, NativeHandle pipeWrite, NativeHandle outFd,
                          NativeHandle inFd, off_t& offset, std::size_t count) noexcept {
  auto* ring = static_cast<struct io_uring*>(ioRing);

  // Step 1: splice from file → pipe_write (at file offset).
  // Submit and wait synchronously — no linked SQEs to avoid ECANCELED issues.
  auto* sqe1 = ::io_uring_get_sqe(ring);
  if (sqe1 == nullptr) [[unlikely]] {
    errno = EAGAIN;
    return -1;
  }
  ::io_uring_prep_splice(sqe1, inFd, offset, pipeWrite, -1, static_cast<unsigned>(count), SPLICE_F_MOVE);
  ::io_uring_sqe_set_data64(sqe1, 0);
  const int submitted1 = ::io_uring_submit(ring);
  if (submitted1 < 0) [[unlikely]] {
    errno = -submitted1;
    return -1;
  }
  struct io_uring_cqe* cqe1 = nullptr;
  const int wait1 = ::io_uring_wait_cqe(ring, &cqe1);
  if (wait1 < 0) [[unlikely]] {
    errno = -wait1;
    return -1;
  }
  const int pipeBytes = cqe1->res;
  ::io_uring_cqe_seen(ring, cqe1);
  if (pipeBytes <= 0) {
    if (pipeBytes < 0) {
      errno = -pipeBytes;
      return -1;
    }
    return 0;  // EOF on file
  }

  // Step 2: splice from pipe_read → socket.
  // Use exactly the bytes that landed in the pipe, and SPLICE_F_NONBLOCK
  // so we return EAGAIN when the socket send buffer is full.
  auto* sqe2 = ::io_uring_get_sqe(ring);
  if (sqe2 == nullptr) [[unlikely]] {
    errno = EAGAIN;
    return -1;
  }
  ::io_uring_prep_splice(sqe2, pipeRead, -1, outFd, -1, static_cast<unsigned>(pipeBytes),
                         SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
  ::io_uring_sqe_set_data64(sqe2, 0);
  const int submitted2 = ::io_uring_submit(ring);
  if (submitted2 < 0) [[unlikely]] {
    errno = -submitted2;
    return -1;
  }
  struct io_uring_cqe* cqe2 = nullptr;
  const int wait2 = ::io_uring_wait_cqe(ring, &cqe2);
  if (wait2 < 0) [[unlikely]] {
    errno = -wait2;
    return -1;
  }
  const int socketBytes = cqe2->res;
  ::io_uring_cqe_seen(ring, cqe2);
  if (socketBytes < 0) {
    errno = -socketBytes;
    return -1;
  }

  offset += socketBytes;
  return static_cast<int64_t>(socketBytes);
}
#endif

}  // namespace aeronet
