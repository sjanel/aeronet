#pragma once

// Platform detection, portable type aliases and error handling for aeronet's system layer.
//
// Detection macros:
//   AERONET_LINUX   – defined on Linux
//   AERONET_MACOS   – defined on macOS / Darwin
//   AERONET_WINDOWS – defined on Windows (MSVC / MinGW)
//   AERONET_POSIX   – defined on any POSIX-compliant OS (Linux, macOS, *BSD …)
//
// Portable types / functions:
//   NativeHandle       – the OS handle type for sockets / file descriptors
//   kInvalidHandle     – sentinel value representing an invalid handle
//   LastSystemError()  – retrieve the last system/socket error code
//   SystemErrorMessage – human-readable description for an error code
//   CloseNativeHandle  – close a socket / file descriptor portably
//
// Portable error constants (namespace aeronet::error):
//   kWouldBlock, kInterrupted, kInProgress, kAlready, kConnectionReset,
//   kConnectionAborted, kBrokenPipe, kNoBufferSpace, kNotSupported,
//   kTooManyFiles

#ifdef __linux__
#define AERONET_LINUX
#define AERONET_POSIX
#elifdef __APPLE__
#include <TargetConditionals.h>
#define AERONET_MACOS
#define AERONET_POSIX
#elifdef _WIN32
#define AERONET_WINDOWS
#else
#error "Unsupported platform – aeronet currently supports Linux, macOS and Windows"
#endif

#ifdef AERONET_WINDOWS
#include <winsock2.h>  // SOCKET, INVALID_SOCKET, closesocket, WSAE*
#else
#include <unistd.h>  // close

#include <cerrno>  // errno, EAGAIN, EINTR, …
#endif

#include <cstring>  // std::strerror

// ---------------------------------------------------------------------------
// ssize_t – signed size type (POSIX provides it; Windows does not).
// ---------------------------------------------------------------------------
#ifdef AERONET_WINDOWS
#include <cstddef>  // std::ptrdiff_t
using ssize_t = std::ptrdiff_t;
#endif

namespace aeronet {

// ---------------------------------------------------------------------------
// NativeHandle – the OS-level socket / file descriptor type.
//   POSIX : int            (file descriptors)
//   Windows : SOCKET       (unsigned pointer-sized integer)
// ---------------------------------------------------------------------------
#ifdef AERONET_WINDOWS
using NativeHandle = SOCKET;
inline constexpr NativeHandle kInvalidHandle = INVALID_SOCKET;
#else
using NativeHandle = int;
inline constexpr NativeHandle kInvalidHandle = -1;
#endif

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
// SystemErrorMessage – human-readable description for an error code.
//   POSIX  : wraps std::strerror (safe for errno values).
//   Windows: uses FormatMessageA for system/Winsock error codes (10 000+),
//            falls back to std::strerror for CRT errno values.
//   The returned pointer is valid at least until the next call from the same
//   thread.  Always log the numeric code alongside the message.
// ---------------------------------------------------------------------------
#ifdef AERONET_WINDOWS
inline const char* SystemErrorMessage(int err) noexcept {
  // Winsock / Win32 error codes are >= 10000; std::strerror only knows CRT errno.
  if (err >= 10000) {
    thread_local char buf[256];
    DWORD len = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                 static_cast<DWORD>(err), 0, buf, sizeof(buf), nullptr);
    // Strip trailing \r\n added by FormatMessage.
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
      buf[--len] = '\0';
    }
    if (len > 0) {
      return buf;
    }
    // FormatMessage failed; fall back to a generic message.
    return "Unknown system error";
  }
  return std::strerror(err);
}
#else
inline const char* SystemErrorMessage(int err) noexcept { return std::strerror(err); }
#endif

// ---------------------------------------------------------------------------
// CloseNativeHandle – close a socket / file descriptor portably.
//   POSIX : ::close(fd)
//   Windows: ::closesocket(fd)
// Returns 0 on success, -1 / SOCKET_ERROR on failure.
// ---------------------------------------------------------------------------
#ifdef AERONET_WINDOWS
inline int CloseNativeHandle(NativeHandle fd) noexcept { return ::closesocket(fd); }
#else
inline int CloseNativeHandle(NativeHandle fd) noexcept { return ::close(fd); }
#endif

// ---------------------------------------------------------------------------
// Portable error-code constants for socket / system operations.
// On POSIX these map to the standard errno values; on Windows they map to
// the corresponding WSA* codes returned by WSAGetLastError().
// For CRT-only error codes that are identical on all platforms (e.g. EINTR
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
