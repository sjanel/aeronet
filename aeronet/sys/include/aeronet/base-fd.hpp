#pragma once

#include "aeronet/native-handle.hpp"

#ifdef AERONET_WINDOWS
#include <cstdint>
#endif

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

#ifdef AERONET_MACOS
  /// Create a non-owning wrapper around an existing fd.
  /// The returned BaseFd will NOT close the fd on destruction or move-assignment.
  /// Used for the macOS shared-listener model where multiple event loops watch
  /// the same listen fd but only the original owner closes it.
  static BaseFd Borrow(NativeHandle fd) noexcept {
    BaseFd result;
    result._fd = fd;
    result._owns = false;
    return result;
  }
#endif

#ifdef AERONET_WINDOWS
  /// Discriminates between Winsock SOCKETs and generic Win32 HANDLEs.
  enum class HandleKind : uint8_t { Socket, Win32Handle };

  explicit BaseFd(NativeHandle fd = kClosedFd, HandleKind kind = HandleKind::Socket) noexcept : _fd(fd), _kind(kind) {}
#else
  explicit BaseFd(NativeHandle fd = kClosedFd) noexcept : _fd(fd) {}
#endif

  BaseFd(const BaseFd& other) = delete;
  BaseFd(BaseFd&& other) noexcept
      : _fd(other.release())
#ifdef AERONET_MACOS
        ,
        _owns(other._owns)
#endif
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

  // Equality comparison - compare only the underlying fd, not ownership semantics.
  bool operator==(const BaseFd& other) const noexcept { return _fd == other._fd; }

 private:
  NativeHandle _fd;
#ifdef AERONET_MACOS
  bool _owns{true};
#endif
#ifdef AERONET_WINDOWS
  HandleKind _kind = HandleKind::Socket;
#endif
};

}  // namespace aeronet
