#pragma once

#include <cstdint>

#include "aeronet/async-resume-poster.hpp"

namespace aeronet {

// What the awaitables of a request read and update while its async handler runs: why its coroutine suspended, which
// tells the protocol running the handler when to resume it, and how to resume it from another thread. Owned by the
// protocol (per HTTP/1.1 connection or per HTTP/2 stream), which points the request to it.
struct AsyncAwaitState {
  enum class Reason : uint8_t {
    None,                // running, completed, or suspended by an awaitable that does not need the event loop
    WaitingForBody,      // waits for the rest of the request body (HTTP/1.1)
    WaitingForCallback,  // waits for the completion of deferred work, running in the background
  };

  AsyncResumePoster resumePoster;
  Reason reason{Reason::None};
  // The request body cannot be read yet (HTTP/1.1): it is still being received, or was received during deferred work,
  // whose completion it waits for. The body awaitables suspend until then. Not modified while deferred work runs.
  bool needsBody{false};
};

}  // namespace aeronet
