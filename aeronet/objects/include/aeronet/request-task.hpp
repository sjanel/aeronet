#pragma once

#include <coroutine>
#include <exception>
#include <utility>

namespace aeronet {

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
