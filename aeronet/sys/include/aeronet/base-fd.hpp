#pragma once

#include "aeronet/platform.hpp"

namespace aeronet {

// Simple RAII class wrapping a socket file descriptor / native handle.
// On POSIX this wraps an int fd; on Windows it wraps a SOCKET.
//
// On Windows, some OS objects (Event, WaitableTimer) are Win32 HANDLEs rather
// than SOCKETs. Construct with HandleKind::Win32Handle so that close() calls
// CloseHandle() instead of closesocket().
class BaseFd {
 public:
  static constexpr NativeHandle kClosedFd = kInvalidHandle;

#ifdef AERONET_WINDOWS
  /// Discriminates between Winsock SOCKETs and generic Win32 HANDLEs.
  enum class HandleKind : uint8_t { Socket, Win32Handle };
#endif

#ifdef AERONET_WINDOWS
  explicit BaseFd(NativeHandle fd = kClosedFd, HandleKind kind = HandleKind::Socket) noexcept : _fd(fd), _kind(kind) {}
#else
  explicit BaseFd(NativeHandle fd = kClosedFd) noexcept : _fd(fd) {}
#endif

  BaseFd(const BaseFd& other) = delete;
  BaseFd(BaseFd&& other) noexcept
      : _fd(other.release())
#ifdef AERONET_WINDOWS
        ,
        _kind(other._kind)
#endif
  {
  }
  BaseFd& operator=(const BaseFd& other) = delete;
  BaseFd& operator=(BaseFd&& other) noexcept;

  ~BaseFd() { close(); }

  [[nodiscard]] NativeHandle fd() const noexcept { return _fd; }

  // Truthy check so users can write: if (baseFd) { ... }
  // Returns true if the underlying fd is valid (not closed).
  explicit operator bool() const noexcept { return _fd != kClosedFd; }

  // Release ownership of the underlying fd without closing it.
  // Returns the raw fd and sets this object to closed state.
  [[nodiscard]] NativeHandle release() noexcept;

  // Close the underlying file descriptor immediately.
  // Typically you should rely on RAII (destructor) except when you need to:
  //  * perform an early shutdown before object lifetime ends (e.g. SingleHttpServer::stop())
  //  * observe/force close errors deterministically at a specific point
  // Idempotent: multiple calls after first successful/failed close are no-ops.
  void close() noexcept;

  // Equality comparison - simply compare the underlying fd integer.
  bool operator==(const BaseFd&) const noexcept = default;

 private:
  NativeHandle _fd;
#ifdef AERONET_WINDOWS
  HandleKind _kind = HandleKind::Socket;
#endif
};

}  // namespace aeronet
