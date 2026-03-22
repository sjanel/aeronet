#pragma once

#include <cstddef>
#include <cstdint>

#ifdef AERONET_IO_URING
#include <sys/types.h>  // off_t
#endif

#include "aeronet/native-handle.hpp"

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
// Windows: uses TransmitFile.  `fileFd` is a CRT file descriptor (from
//          _open / open) — it is converted internally via _get_osfhandle.
int64_t Sendfile(NativeHandle outFd, int fileFd, std::size_t& offset, std::size_t count) noexcept;

#ifdef AERONET_IO_URING
/// io_uring splice-based sendfile replacement.
/// Uses two linked IORING_OP_SPLICE SQEs (file → pipe → socket) for zero-copy transfer.
/// Returns bytes transferred (>= 0) or -1 on error (errno set).
int64_t IoUringSpliceFile(void* ioRing, NativeHandle pipeRead, NativeHandle pipeWrite, NativeHandle outFd,
                          NativeHandle inFd, off_t& offset, std::size_t count) noexcept;
#endif

}  // namespace aeronet
