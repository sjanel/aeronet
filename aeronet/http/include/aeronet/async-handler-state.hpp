#pragma once

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "aeronet/http-response.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class CorsPolicy;

class OwnedCoroutineHandle {
 public:
  OwnedCoroutineHandle() = default;

  OwnedCoroutineHandle(const OwnedCoroutineHandle&) = delete;
  OwnedCoroutineHandle& operator=(const OwnedCoroutineHandle&) = delete;

  OwnedCoroutineHandle(OwnedCoroutineHandle&& other) noexcept = delete;

  OwnedCoroutineHandle& operator=(OwnedCoroutineHandle&& other) noexcept {
    assert(this != &other);
    reset();
    _handle = std::exchange(other._handle, {});
    return *this;
  }

  OwnedCoroutineHandle& operator=(std::coroutine_handle<> handle) noexcept {
    assert(_handle != handle);
    reset();
    _handle = std::move(handle);
    return *this;
  }

  ~OwnedCoroutineHandle() { reset(); }

  void reset() noexcept {
    if (_handle) {
      _handle.destroy();
      _handle = {};
    }
  }

  [[nodiscard]] std::coroutine_handle<> release() noexcept { return std::exchange(_handle, std::coroutine_handle<>{}); }

  [[nodiscard]] bool done() const noexcept { return _handle.done(); }

  void resume() const { _handle.resume(); }

  explicit operator bool() const noexcept { return static_cast<bool>(_handle); }

  friend bool operator==(const OwnedCoroutineHandle& lhs, std::coroutine_handle<> rhs) noexcept {
    return lhs._handle == rhs;
  }

 private:
  std::coroutine_handle<> _handle;
};

struct AsyncHandlerState {
  AsyncHandlerState() = default;

  enum class AwaitReason : uint8_t { None, WaitingForBody, WaitingForCallback };

  // Tells whether the coroutine waits for the completion of deferred work, still running in the background.
  [[nodiscard]] bool isAwaitingCallback() const noexcept {
    return handle && awaitReason == AwaitReason::WaitingForCallback;
  }

  // Tells whether the connection input must be left untouched: the whole request of the running handler was received
  // (the input buffer holds its body), or the request was abandoned while deferred work may still use it.
  [[nodiscard]] bool holdsInput() const noexcept { return active ? !needsBody : static_cast<bool>(handle); }

  // Resets this state, destroying the coroutine frame first: it may still use the handler.
  void clear() noexcept {
    handle.reset();
    *this = {};
  }

  // Keeps the handler (whose captures the coroutine may use) alive until the coroutine is destroyed, even if the router
  // is updated meanwhile. Declared before the handle so that it is destroyed after it.
  std::shared_ptr<const void> handlerKeepAlive;
  // stable storage for the current request head when async body progress is needed
  RawChars headBuffer;
  OwnedCoroutineHandle handle;
  AwaitReason awaitReason{AwaitReason::None};
  bool active{false};
  bool needsBody{false};
  bool usesSharedDecompressedBody{false};
  bool isChunked{false};
  // True once the handler outlived its dispatch (suspended, or waiting for its body): the router may have been updated
  // since, and the CORS policy and response middleware of its route must then be looked up again.
  bool routeMayHaveChanged{false};
  uint32_t responseMiddlewareCount{0};
  std::size_t consumedBytes{0};
  // Per-route maximum body size override (MAX = use global limit only).
  std::size_t maxBodyBytes = static_cast<std::size_t>(-1);
  const CorsPolicy* corsPolicy{nullptr};
  const void* responseMiddleware{nullptr};
  std::optional<HttpResponse> pendingResponse;
  // Callback to post async work completion to the server's event loop.
  // Set by the server when dispatching an async handler.
  std::function<void(std::coroutine_handle<>, std::function<void()>)> postCallback;
};

using AsyncHandlerStatePool = ObjectPool<AsyncHandlerState>;

}  // namespace aeronet