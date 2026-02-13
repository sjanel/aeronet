#pragma once

#include <sys/socket.h>  // sockaddr_storage

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
bool SetNonBlocking(int fd) noexcept;

// Set the close-on-exec flag on a file descriptor.
// Returns true on success.
bool SetCloseOnExec(int fd) noexcept;

// Enable TCP_NODELAY (disable Nagle's algorithm) on a TCP socket.
// Returns true on success.
bool SetTcpNoDelay(int fd) noexcept;

// Retrieve the pending socket error (SO_ERROR).
// Returns the error code (0 means no error, >0 is errno).
// On failure to query, returns the errno from getsockopt itself.
int GetSocketError(int fd) noexcept;

// Fill `addr` with the local address bound to `fd`.
// Returns true on success.
bool GetLocalAddress(int fd, sockaddr_storage& addr) noexcept;

// Fill `addr` with the remote peer address of `fd`.
// Returns true on success.
bool GetPeerAddress(int fd, sockaddr_storage& addr) noexcept;

// Determine whether a sockaddr_storage represents a loopback address.
// Supports AF_INET (127.0.0.0/8) and AF_INET6 (::1).
bool IsLoopback(const sockaddr_storage& addr) noexcept;

// Send data on a connected socket with platform-appropriate flags
// (MSG_NOSIGNAL on Linux, SO_NOSIGPIPE on macOS, none on Windows).
// Non-blocking: the socket must already be in non-blocking mode or
// the caller must accept blocking behaviour.
// Returns the number of bytes sent, or -1 on error (errno is set).
int64_t SafeSend(int fd, const void* data, std::size_t len) noexcept;

// Convenience overload accepting a string_view.
inline int64_t SafeSend(int fd, std::string_view data) noexcept { return SafeSend(fd, data.data(), data.size()); }

}  // namespace aeronet
