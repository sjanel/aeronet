#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <type_traits>
#include <utility>

namespace aeronet {

// Types that can be awaited in a RequestTask coroutine: the awaitables of aeronet (HttpRequestView::bodyAwaitable(),
// readBodyAsync() and deferWork(), declaring an AeronetAwaitableTag type), std::suspend_always and std::suspend_never.
// The server resumes right away a coroutine suspended by any other awaitable: one resuming the coroutine by itself
// (from another thread for instance) would resume it twice. A custom awaitable may declare AeronetAwaitableTag if it
// never suspends, or if it resumes the coroutine before returning from its await_suspend().
template <class Awaitable>
concept RequestTaskAwaitable = requires {
  typename Awaitable::AeronetAwaitableTag;
} || std::same_as<Awaitable, std::suspend_always> || std::same_as<Awaitable, std::suspend_never>;

namespace detail {

template <class Awaitable>
Awaitable&& CheckRequestTaskAwaitable(Awaitable&& awaitable) noexcept {
  static_assert(RequestTaskAwaitable<std::remove_cvref_t<Awaitable>>,
                "Unsupported awaitable in a RequestTask coroutine (see RequestTaskAwaitable)");
  return std::forward<Awaitable>(awaitable);
}

}  // namespace detail

template <class T>
class RequestTask {
 public:
  struct promise_type {
    RequestTask get_return_object() noexcept {
      return RequestTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_value(T value) noexcept(std::is_nothrow_move_constructible_v<T>) { _value = std::move(value); }

    template <class Awaitable>
    Awaitable&& await_transform(Awaitable&& awaitable) noexcept {
      return detail::CheckRequestTaskAwaitable(std::forward<Awaitable>(awaitable));
    }

    void unhandled_exception() noexcept { _exception = std::current_exception(); }

    T&& consume_result() {
      if (_exception) {
        std::rethrow_exception(_exception);
      }
      return std::move(_value);
    }

    std::exception_ptr _exception;
    T _value{};
  };

  RequestTask() noexcept = default;
  explicit RequestTask(std::coroutine_handle<promise_type> handle) noexcept : _coro(handle) {}

  RequestTask(RequestTask&& other) noexcept : _coro(std::exchange(other._coro, {})) {}
  RequestTask& operator=(RequestTask&& other) noexcept {
    if (this != &other) {
      reset();
      _coro = std::exchange(other._coro, {});
    }
    return *this;
  }

  RequestTask(const RequestTask&) = delete;
  RequestTask& operator=(const RequestTask&) = delete;

  ~RequestTask() { reset(); }

  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(_coro); }
  [[nodiscard]] bool done() const noexcept { return !_coro || _coro.done(); }

  /// Returns the address of the internal coroutine handle (opaque pointer for identity comparison).
  [[nodiscard]] void* coroutineAddress() const noexcept { return _coro ? _coro.address() : nullptr; }

  void resume() {
    if (_coro) {
      _coro.resume();
    }
  }

  T runSynchronously() {
    while (_coro && !_coro.done()) {
      _coro.resume();
    }
    return std::move(_coro.promise().consume_result());
  }

  void reset() noexcept {
    if (_coro) {
      _coro.destroy();
      _coro = {};
    }
  }

  [[nodiscard]] std::coroutine_handle<promise_type> release() noexcept { return std::exchange(_coro, {}); }

 private:
  std::coroutine_handle<promise_type> _coro;
};

template <>
class RequestTask<void> {
 public:
  struct promise_type {
    RequestTask get_return_object() noexcept {
      return RequestTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() const noexcept {}

    template <class Awaitable>
    Awaitable&& await_transform(Awaitable&& awaitable) noexcept {
      return detail::CheckRequestTaskAwaitable(std::forward<Awaitable>(awaitable));
    }
    void unhandled_exception() noexcept { _exception = std::current_exception(); }

    void rethrow_if_needed() const {
      if (_exception) {
        std::rethrow_exception(_exception);
      }
    }

    std::exception_ptr _exception;
  };

  RequestTask() noexcept = default;
  explicit RequestTask(std::coroutine_handle<promise_type> handle) noexcept : _coro(handle) {}

  RequestTask(RequestTask&& other) noexcept : _coro(std::exchange(other._coro, {})) {}
  RequestTask& operator=(RequestTask&& other) noexcept {
    if (this != &other) {
      reset();
      _coro = std::exchange(other._coro, {});
    }
    return *this;
  }

  RequestTask(const RequestTask&) = delete;
  RequestTask& operator=(const RequestTask&) = delete;

  ~RequestTask() { reset(); }

  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(_coro); }
  [[nodiscard]] bool done() const noexcept { return !_coro || _coro.done(); }

  /// Returns the address of the internal coroutine handle (opaque pointer for identity comparison).
  [[nodiscard]] void* coroutineAddress() const noexcept { return _coro ? _coro.address() : nullptr; }

  void resume() {
    if (_coro) {
      _coro.resume();
    }
  }

  void runSynchronously() {
    while (_coro && !_coro.done()) {
      _coro.resume();
    }
    if (_coro) {
      _coro.promise().rethrow_if_needed();
    }
  }

  void reset() noexcept {
    if (_coro) {
      _coro.destroy();
      _coro = {};
    }
  }

  [[nodiscard]] std::coroutine_handle<promise_type> release() noexcept { return std::exchange(_coro, {}); }

 private:
  std::coroutine_handle<promise_type> _coro;
};

}  // namespace aeronet
