#pragma once

#ifdef AERONET_WINDOWS
#include <winsock2.h>  // SOCKET, INVALID_SOCKET, closesocket, WSAE*
#else
#include <cerrno>  // errno, EAGAIN, EINTR, …
#endif

namespace aeronet {

// ---------------------------------------------------------------------------
// LastSystemError – retrieve the last system/socket error code.
//   POSIX : errno
//   Windows: WSAGetLastError()
// For CRT file-I/O functions that set errno on all platforms (open, pread,
// lseek …), read errno directly instead.
// ---------------------------------------------------------------------------
#ifdef AERONET_WINDOWS
inline int LastSystemError() noexcept { return WSAGetLastError(); }
#else
inline int LastSystemError() noexcept { return errno; }
#endif

// ---------------------------------------------------------------------------
// Portable error-code constants for socket / system operations.
// On POSIX these map to the standard errno values; on Windows they map to
// the corresponding WSA* codes returned by WSAGetLastError().
// For CRT-only error codes that are identical on all platforms (e.g. error::kInterrupted
// for file I/O), use the <cerrno> constant directly.
// ---------------------------------------------------------------------------
namespace error {
#ifdef AERONET_WINDOWS
inline constexpr int kWouldBlock = WSAEWOULDBLOCK;
inline constexpr int kInterrupted = WSAEINTR;
inline constexpr int kInProgress = WSAEINPROGRESS;
inline constexpr int kAlready = WSAEALREADY;
inline constexpr int kConnectionReset = WSAECONNRESET;
inline constexpr int kConnectionAborted = WSAECONNABORTED;
inline constexpr int kBrokenPipe = WSAECONNRESET;  // Windows has no EPIPE for sockets
inline constexpr int kNoBufferSpace = WSAENOBUFS;
inline constexpr int kNotSupported = WSAEOPNOTSUPP;
inline constexpr int kTooManyFiles = WSAEMFILE;
#else
static_assert(EAGAIN == EWOULDBLOCK, "Check that kWouldBlock is set to the correct value for this platform");
static_assert(EOPNOTSUPP == ENOTSUP);

inline constexpr int kWouldBlock = EAGAIN;
inline constexpr int kInterrupted = EINTR;
inline constexpr int kInProgress = EINPROGRESS;
inline constexpr int kAlready = EALREADY;
inline constexpr int kConnectionReset = ECONNRESET;
inline constexpr int kConnectionAborted = ECONNABORTED;
inline constexpr int kBrokenPipe = EPIPE;
inline constexpr int kNoBufferSpace = ENOBUFS;
inline constexpr int kNotSupported = EOPNOTSUPP;
inline constexpr int kTooManyFiles = EMFILE;
#endif
}  // namespace error

}  // namespace aeronet
