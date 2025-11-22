#pragma once

#include <dlfcn.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aeronet::sys::test_support {

// Allocation failure injection utilities used by sys tests. Tests can call
// `FailNextMalloc()` / `FailNextRealloc()` to cause the next N allocations
// to return ENOMEM. These helpers also provide portable resolver helpers to
// obtain the real `malloc`/`realloc` implementations via RTLD_NEXT when
// needed. Do NOT override `free` from here — that can break the dynamic
// loader and sanitizer internals.

// Use simple inline counters and atomic builtins to avoid C++ runtime
// initialization that can be unsafe when allocator hooks run during
// dynamic loader/sanitizer initialization.
inline int g_malloc_failure_counter = 0;
inline int g_realloc_failure_counter = 0;

__attribute__((no_sanitize("address"))) inline void FailNextMalloc(int count = 1) {
  __atomic_store_n(&g_malloc_failure_counter, count, __ATOMIC_RELAXED);
}

__attribute__((no_sanitize("address"))) inline void FailNextRealloc(int count = 1) {
  __atomic_store_n(&g_realloc_failure_counter, count, __ATOMIC_RELAXED);
}

[[nodiscard]] inline __attribute__((no_sanitize("address"))) bool ShouldFailMalloc() {
  int remaining = __atomic_load_n(&g_malloc_failure_counter, __ATOMIC_RELAXED);
  while (remaining > 0) {
    int desired = remaining - 1;
    if (__atomic_compare_exchange_n(&g_malloc_failure_counter, &remaining, desired, /*weak=*/true, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] inline __attribute__((no_sanitize("address"))) bool ShouldFailRealloc() {
  int remaining = __atomic_load_n(&g_realloc_failure_counter, __ATOMIC_RELAXED);
  while (remaining > 0) {
    int desired = remaining - 1;
    if (__atomic_compare_exchange_n(&g_realloc_failure_counter, &remaining, desired, /*weak=*/true, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED)) {
      return true;
    }
  }
  return false;
}

// Portable resolver for RTLD_NEXT symbols. This is inline to allow inclusion
// in test translation units. It aborts if symbol resolution fails.
template <typename Fn>
inline Fn ResolveNext(const char* name) {
  void* sym = dlsym(RTLD_NEXT, name);
  if (sym == nullptr) {
    std::abort();
  }
  return reinterpret_cast<Fn>(sym);
}

inline int CreateMemfd(std::string_view name) {
  const std::string nameStr(name);
  int retfd = static_cast<int>(::syscall(SYS_memfd_create, nameStr.c_str(), MFD_CLOEXEC));
  if (retfd < 0) {
    throw std::runtime_error("memfd_create failed: " + std::string(std::strerror(errno)));
  }
  return retfd;
}

template <typename Action>
class ActionQueue {
 public:
  ActionQueue() = default;
  ActionQueue(const ActionQueue&) = delete;
  ActionQueue& operator=(const ActionQueue&) = delete;

  void reset() {
    std::scoped_lock<std::mutex> lock(_mutex);
    _actions.clear();
  }

  void setActions(std::initializer_list<Action> actions) {
    std::scoped_lock<std::mutex> lock(_mutex);
    _actions.assign(actions.begin(), actions.end());
  }

  void setActions(std::vector<Action> actions) {
    std::scoped_lock<std::mutex> lock(_mutex);
    _actions.clear();
    for (auto& action : actions) {
      _actions.emplace_back(std::move(action));
    }
  }

  void push(Action action) {
    std::scoped_lock<std::mutex> lock(_mutex);
    _actions.emplace_back(std::move(action));
  }

  [[nodiscard]] std::optional<Action> pop() {
    std::scoped_lock<std::mutex> lock(_mutex);
    if (_actions.empty()) {
      return std::nullopt;
    }
    Action front = std::move(_actions.front());
    _actions.pop_front();
    return front;
  }

 private:
  std::mutex _mutex;
  std::deque<Action> _actions;
};

template <typename Key, typename Action, typename Hash = std::hash<Key>, typename Eq = std::equal_to<Key>>
class KeyedActionQueue {
 public:
  KeyedActionQueue() = default;
  KeyedActionQueue(const KeyedActionQueue&) = delete;
  KeyedActionQueue& operator=(const KeyedActionQueue&) = delete;

  void reset() {
    std::scoped_lock<std::mutex> lock(_mutex);
    _queues.clear();
  }

  void setActions(const Key& key, std::initializer_list<Action> actions) {
    std::scoped_lock<std::mutex> lock(_mutex);
    _queues[key] = std::deque<Action>(actions.begin(), actions.end());
  }

  void setActions(const Key& key, std::vector<Action> actions) {
    std::scoped_lock<std::mutex> lock(_mutex);
    auto& queue = _queues[key];
    queue.clear();
    for (auto& action : actions) {
      queue.emplace_back(std::move(action));
    }
  }

  void push(const Key& key, Action action) {
    std::scoped_lock<std::mutex> lock(_mutex);
    _queues[key].emplace_back(std::move(action));
  }

  [[nodiscard]] std::optional<Action> pop(const Key& key) {
    std::scoped_lock<std::mutex> lock(_mutex);
    auto it = _queues.find(key);
    if (it == _queues.end() || it->second.empty()) {
      return std::nullopt;
    }
    Action front = std::move(it->second.front());
    it->second.pop_front();
    if (it->second.empty()) {
      _queues.erase(it);
    }
    return front;
  }

 private:
  std::mutex _mutex;
  std::unordered_map<Key, std::deque<Action>, Hash, Eq> _queues;
};

template <typename Queue>
class QueueResetGuard {
 public:
  explicit QueueResetGuard(Queue& queue) noexcept : _queue(queue) {}
  QueueResetGuard(const QueueResetGuard&) = delete;
  QueueResetGuard& operator=(const QueueResetGuard&) = delete;
  ~QueueResetGuard() { _queue.reset(); }

 private:
  Queue& _queue;
};

}  // namespace aeronet::sys::test_support
