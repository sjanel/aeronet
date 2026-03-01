#pragma once

#include "aeronet/platform.hpp"

#ifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#else
#include <sys/socket.h>  // sockaddr_storage
#endif

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aeronet {

// Cross-platform socket operations.
// All Linux / macOS / Windows specifics are hidden in the .cpp file.
// These thin wrappers centralise system calls so that
// higher-level modules (http, main, objects …) never include
// platform networking headers directly.

// Set a file descriptor to non-blocking mode.
// Returns true on success.
bool SetNonBlocking(NativeHandle fd) noexcept;

// Set the close-on-exec flag on a file descriptor.
// Returns true on success. No-op on Windows.
bool SetCloseOnExec(NativeHandle fd) noexcept;

// Suppress SIGPIPE on a socket (macOS: SO_NOSIGPIPE).
// No-op on Linux (uses MSG_NOSIGNAL per-send) and Windows.
// Returns true on success.
bool SetNoSigPipe(NativeHandle fd) noexcept;

#ifdef AERONET_POSIX
// Set both ends of a pipe to non-blocking mode and close-on-exec.
void SetPipeNonBlockingCloExec(int pipeRd, int pipeWr) noexcept;
#endif

// Enable TCP_NODELAY (disable Nagle's algorithm) on a TCP socket.
// Returns true on success.
bool SetTcpNoDelay(NativeHandle fd) noexcept;

// Retrieve the pending socket error (SO_ERROR).
// Returns the error code (0 means no error, >0 is errno).
// On failure to query, returns the errno from getsockopt itself.
int GetSocketError(NativeHandle fd) noexcept;

// Fill `addr` with the local address bound to `fd`.
// Returns true on success.
bool GetLocalAddress(NativeHandle fd, sockaddr_storage& addr) noexcept;

// Fill `addr` with the remote peer address of `fd`.
// Returns true on success.
bool GetPeerAddress(NativeHandle fd, sockaddr_storage& addr) noexcept;

// Determine whether a sockaddr_storage represents a loopback address.
// Supports AF_INET (127.0.0.0/8) and AF_INET6 (::1).
bool IsLoopback(const sockaddr_storage& addr) noexcept;

// Send data on a connected socket with platform-appropriate flags
// (MSG_NOSIGNAL on Linux, SO_NOSIGPIPE on macOS, none on Windows).
// Non-blocking: the socket must already be in non-blocking mode or
// the caller must accept blocking behaviour.
// Returns the number of bytes sent, or -1 on error (errno is set).
int64_t SafeSend(NativeHandle fd, const void* data, std::size_t len) noexcept;

// Convenience overload accepting a string_view.
inline int64_t SafeSend(NativeHandle fd, std::string_view data) noexcept {
  return SafeSend(fd, data.data(), data.size());
}

// Shutdown the write half of a socket connection.
// Returns true on success, false on error (errno is set).
bool ShutdownWrite(NativeHandle fd) noexcept;

// Shutdown both read and write halves of a socket connection.
// Returns true on success, false on error (errno is set).
bool ShutdownReadWrite(NativeHandle fd) noexcept;

}  // namespace aeronet
