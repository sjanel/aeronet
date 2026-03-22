#pragma once

#include "aeronet/native-handle.hpp"

// Indirection point for io_uring data-path I/O checks. With io_uring, the
// kernel performs send/recv/splice directly, bypassing libc — so
// LD_PRELOAD-style test overrides of write()/writev()/sendfile() never fire.
// The library provides a default that always returns true; unit tests override
// it at link-time to disable io_uring data I/O for specific fds that have
// pending error-injection actions.
extern "C" bool AeronetUseIoRingForFd(aeronet::NativeHandle fd);
