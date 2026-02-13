#pragma once

#include <sys/types.h>  // off_t, ssize_t

#include <cstddef>
#include <cstdint>

namespace aeronet {

// Platform-abstracted sendfile.
// Transfers up to `count` bytes from file descriptor `inFd` (at `offset`)
// to socket `outFd`.  On success, `offset` is advanced by the number of
// bytes actually sent.
//
// Returns the number of bytes transferred (>= 0) or -1 on error (errno set).
//
// Linux  : wraps sendfile(2) with its native signature.
// macOS  : wraps sendfile(2) with the macOS signature (arguments reversed,
//          len is in/out).
// Windows: not yet implemented – will use TransmitFile.
int64_t Sendfile(int outFd, int inFd, off_t& offset, std::size_t count) noexcept;

}  // namespace aeronet
