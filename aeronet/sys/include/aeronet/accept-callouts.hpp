#pragma once

#include "aeronet/native-handle.hpp"

// Indirection point for accept notification when using completion-based I/O
// (io_uring). With io_uring, the kernel performs accept() directly, bypassing
// libc — so LD_PRELOAD-style test overrides of accept4() never fire.
// The library provides a default no-op; unit tests override it at link-time
// to track accepted fds and install per-fd error actions.
extern "C" void AeronetOnConnectionAccepted(aeronet::NativeHandle fd);
