#pragma once

#include "aeronet/native-handle.hpp"

// Indirection point for io_uring data-path I/O checks. With the io_uring proactor, the
// kernel performs recv/send directly, bypassing libc — so LD_PRELOAD-style test overrides
// of read()/write()/writev() never fire. The library provides a default that always
// returns true; unit tests override it at link-time to disable ring data I/O for specific
// fds that have pending error-injection actions (forcing the synchronous transport path).
extern "C" bool AeronetUseIoRingForFd(aeronet::NativeHandle fd);
