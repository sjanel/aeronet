#pragma once

#include <coroutine>
#include <cstdint>

#include "aeronet/native-handle.hpp"

namespace aeronet {

// Posts the resumption of a suspended coroutine to the event loop running it, from any thread: the deferred work of a
// request copies it on the event loop thread, then invokes it from its background thread once complete.
struct AsyncResumePoster {
  // Throws only on memory exhaustion: the coroutine is then never resumed.
  void operator()(std::coroutine_handle<> handle) const { pPost(*this, handle); }

  void (*pPost)(const AsyncResumePoster& poster, std::coroutine_handle<> handle){nullptr};
  // The server context of the connection (any state in tests).
  void* pContext{nullptr};
  NativeHandle connectionFd{kInvalidHandle};
  uint32_t connectionGeneration{0};
};

}  // namespace aeronet
