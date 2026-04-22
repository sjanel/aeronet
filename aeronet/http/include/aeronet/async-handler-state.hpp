#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "aeronet/http-response.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class CorsPolicy;

class OwnedCoroutineHandle {
 public:
  OwnedCoroutineHandle() = default;
  OwnedCoroutineHandle(std::coroutine_handle<> handle) noexcept : _handle(handle) {}  // NOLINT

  OwnedCoroutineHandle(const OwnedCoroutineHandle&) = delete;
  OwnedCoroutineHandle& operator=(const OwnedCoroutineHandle&) = delete;

  OwnedCoroutineHandle(OwnedCoroutineHandle&& other) noexcept : _handle(std::exchange(other._handle, {})) {}

  OwnedCoroutineHandle& operator=(OwnedCoroutineHandle&& other) noexcept {
    if (this != &other) {
      reset();
      _handle = std::exchange(other._handle, {});
    }
    return *this;
  }

  OwnedCoroutineHandle& operator=(std::coroutine_handle<> handle) noexcept {
    if (_handle != handle) {
      reset();
      _handle = handle;
    }
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
  [[nodiscard]] std::coroutine_handle<> get() const noexcept { return _handle; }
  [[nodiscard]] void* address() const noexcept { return _handle.address(); }
  [[nodiscard]] bool done() const noexcept { return _handle.done(); }
  void resume() const { _handle.resume(); }

  explicit operator bool() const noexcept { return static_cast<bool>(_handle); }

  friend bool operator==(const OwnedCoroutineHandle& lhs, std::coroutine_handle<> rhs) noexcept {
    return lhs._handle == rhs;
  }

  friend bool operator==(std::coroutine_handle<> lhs, const OwnedCoroutineHandle& rhs) noexcept { return rhs == lhs; }

 private:
  std::coroutine_handle<> _handle;
};

struct AsyncHandlerState {
  AsyncHandlerState() = default;

  enum class AwaitReason : uint8_t { None, WaitingForBody, WaitingForCallback };

  OwnedCoroutineHandle handle;
  // stable storage for the current request head when async body progress is needed
  RawChars headBuffer;
  AwaitReason awaitReason{AwaitReason::None};
  bool active{false};
  bool needsBody{false};
  bool usesSharedDecompressedBody{false};
  bool isChunked{false};
  bool expectContinue{false};
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