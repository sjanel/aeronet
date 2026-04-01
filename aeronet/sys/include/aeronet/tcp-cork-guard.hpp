#pragma once

#include "aeronet/native-handle.hpp"
#include "aeronet/socket-ops.hpp"

namespace aeronet {

/// RAII guard that corks a TCP socket on construction and uncorks on destruction.
/// Corking coalesces small writes into full TCP segments, avoiding partial sends
/// when TCP_NODELAY is active. Linux-only (TCP_CORK); no-op on macOS and Windows.
/// The guard is a no-op when fd is kInvalidHandle.
class TcpCorkGuard {
 public:
  explicit TcpCorkGuard(NativeHandle fd) noexcept : _fd(fd) {
    if (_fd != kInvalidHandle) {
      SetTcpCork(_fd, true);
    }
  }

  ~TcpCorkGuard() {
    if (_fd != kInvalidHandle) {
      SetTcpCork(_fd, false);
    }
  }

  TcpCorkGuard(const TcpCorkGuard&) = delete;
  TcpCorkGuard& operator=(const TcpCorkGuard&) = delete;
  TcpCorkGuard(TcpCorkGuard&&) = delete;
  TcpCorkGuard& operator=(TcpCorkGuard&&) = delete;

 private:
  NativeHandle _fd;
};

}  // namespace aeronet
