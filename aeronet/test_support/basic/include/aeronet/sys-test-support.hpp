#pragma once

#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

// Auto-define AERONET_WANT_SYS_OVERRIDES on Linux. This guards all
// Linux-specific system call overrides (epoll, accept4, recvmsg, memfd, etc.)
// that are not available on macOS or Windows.
#ifndef AERONET_WANT_SYS_OVERRIDES
#ifdef __linux__
#define AERONET_WANT_SYS_OVERRIDES 1
#else
#define AERONET_WANT_SYS_OVERRIDES 0
#endif
#endif

#if AERONET_WANT_SYS_OVERRIDES
#include <linux/memfd.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/bio.h>
#include <openssl/ssl.h>
#endif

#ifdef __linux__
#include <linux/errqueue.h>
#include <netinet/in.h>
#endif

#ifdef __GLIBC__
extern "C" void* __libc_malloc(size_t) noexcept;          // NOLINT(bugprone-reserved-identifier)
extern "C" void* __libc_realloc(void*, size_t) noexcept;  // NOLINT(bugprone-reserved-identifier)
#endif

namespace aeronet::test {

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
// Optional: allow tests to skip a number of successful allocations before
// beginning to fail. This enables testing cases where the second (or Nth)
// allocation should fail while previous ones succeed.
inline int g_malloc_fail_after = 0;
inline int g_realloc_fail_after = 0;

__attribute__((no_sanitize("address"))) inline void FailNextMalloc(int count = 1) {
  // Backwards compatible: request `count` immediate failing allocations.
  __atomic_store_n(&g_malloc_fail_after, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_malloc_failure_counter, count, __ATOMIC_RELAXED);
}

// New overload: skip `expectedSuccessfulAllocs` successful mallocs, then
// cause `expectedUnsuccessfulAllocs` subsequent mallocs to fail.
__attribute__((no_sanitize("address"))) inline void FailNextMalloc(int expectedSuccessfulAllocs,
                                                                   int expectedUnsuccessfulAllocs) {
  __atomic_store_n(&g_malloc_fail_after, expectedSuccessfulAllocs, __ATOMIC_RELAXED);
  __atomic_store_n(&g_malloc_failure_counter, expectedUnsuccessfulAllocs, __ATOMIC_RELAXED);
}

// Fail all subsequent malloc calls until reset. Useful for coarse-grained
// failure testing when the exact allocation index is unknown.
__attribute__((no_sanitize("address"))) inline void FailAllMallocs() {
  __atomic_store_n(&g_malloc_fail_after, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_malloc_failure_counter, std::numeric_limits<int>::max(), __ATOMIC_RELAXED);
}

// Reset malloc behavior to normal (no injected failures).
__attribute__((no_sanitize("address"))) inline void ResetToSysMalloc() {
  __atomic_store_n(&g_malloc_fail_after, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_malloc_failure_counter, 0, __ATOMIC_RELAXED);
}

__attribute__((no_sanitize("address"))) inline void FailNextRealloc(int count = 1) {
  __atomic_store_n(&g_realloc_fail_after, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_realloc_failure_counter, count, __ATOMIC_RELAXED);
}

// New overload for realloc
__attribute__((no_sanitize("address"))) inline void FailNextRealloc(int expectedSuccessfulAllocs,
                                                                    int expectedUnsuccessfulAllocs) {
  __atomic_store_n(&g_realloc_fail_after, expectedSuccessfulAllocs, __ATOMIC_RELAXED);
  __atomic_store_n(&g_realloc_failure_counter, expectedUnsuccessfulAllocs, __ATOMIC_RELAXED);
}

// Fail all subsequent realloc calls until reset.
__attribute__((no_sanitize("address"))) inline void FailAllReallocs() {
  __atomic_store_n(&g_realloc_fail_after, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_realloc_failure_counter, std::numeric_limits<int>::max(), __ATOMIC_RELAXED);
}

// Reset realloc behavior to normal (no injected failures).
__attribute__((no_sanitize("address"))) inline void ResetToSysRealloc() {
  __atomic_store_n(&g_realloc_fail_after, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_realloc_failure_counter, 0, __ATOMIC_RELAXED);
}

[[nodiscard]] inline __attribute__((no_sanitize("address"))) bool ShouldFailMalloc() {
  // First, consume any configured successful allocation allowances.
  int after = __atomic_load_n(&g_malloc_fail_after, __ATOMIC_RELAXED);
  while (after > 0) {
    int desired = after - 1;
    if (__atomic_compare_exchange_n(&g_malloc_fail_after, &after, desired, /*weak=*/true, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED)) {
      // This allocation is allowed to succeed.
      return false;
    }
    // `after` was updated with current value by compare_exchange; loop again.
  }

  // No remaining successful-allocation skips; now behave like previous failure counter semantics.
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
  int after = __atomic_load_n(&g_realloc_fail_after, __ATOMIC_RELAXED);
  while (after > 0) {
    int desired = after - 1;
    if (__atomic_compare_exchange_n(&g_realloc_fail_after, &after, desired, /*weak=*/true, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED)) {
      return false;
    }
  }

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
Fn ResolveNext(const char* name) {
  void* sym = dlsym(RTLD_NEXT, name);
  if (sym == nullptr) {
    std::abort();
  }
  return reinterpret_cast<Fn>(sym);
}

}  // namespace aeronet::test

// Disable overriding malloc/realloc for:
// 1. Clang builds instrumented with AddressSanitizer - Clang's ASAN runtime
//    may call allocation functions very early during initialization.
// 2. Non-glibc systems (like musl/Alpine) - without __libc_malloc fallback,
//    dlsym resolution can deadlock or recurse during early initialization.
#if defined(__clang__) && defined(__has_feature)
#if __has_feature(address_sanitizer)
#define AERONET_WANT_MALLOC_OVERRIDES 0
#endif
#endif

#ifndef AERONET_WANT_MALLOC_OVERRIDES
#ifdef __GLIBC__
#define AERONET_WANT_MALLOC_OVERRIDES 1
#else
#define AERONET_WANT_MALLOC_OVERRIDES 0
#endif
#endif

#if AERONET_WANT_MALLOC_OVERRIDES
void* CallRealMalloc(size_t size) {
  using MallocFn = void* (*)(size_t);
  static MallocFn fn = nullptr;
  static volatile int resolving = 0;
  if (fn != nullptr) {
    return fn(size);
  }
  // Try to become the resolver using an atomic CAS builtin (no libc calls)
  if (!__sync_bool_compare_and_swap(&resolving, 0, 1)) {
    // Another resolver in progress; fall back to direct libc symbol to avoid
    // calling dlsym while it's being resolved.
#ifdef __GLIBC__
    return __libc_malloc(size);
#else
    while (fn == nullptr) {
      __asm__ __volatile__("pause");
    }
    return fn(size);
#endif
  }

  // We are the resolver. Resolve the next-in-chain allocator via RTLD_NEXT.
  fn = aeronet::test::ResolveNext<MallocFn>("malloc");
  __sync_synchronize();
  resolving = 0;
  return fn(size);
}

void* CallRealRealloc(void* ptr, size_t size) {
  using ReallocFn = void* (*)(void*, size_t);
  static ReallocFn fn = nullptr;
  static volatile int resolving = 0;
  if (fn != nullptr) {
    return fn(ptr, size);
  }
  if (!__sync_bool_compare_and_swap(&resolving, 0, 1)) {
#ifdef __GLIBC__
    return __libc_realloc(ptr, size);
#else
    while (fn == nullptr) {
      __asm__ __volatile__("pause");
    }
    return fn(ptr, size);
#endif
  }

  fn = aeronet::test::ResolveNext<ReallocFn>("realloc");
  __sync_synchronize();
  resolving = 0;
  return fn(ptr, size);
}
#endif  // AERONET_WANT_MALLOC_OVERRIDES

// Note: we intentionally do NOT override `free` to avoid interfering with
// runtime loader and sanitizer internals which may call `free` during
// dlsym/dlerror initialization. Only `malloc`/`realloc` are overridden for
// deterministic failure injection in tests.

// Provide malloc/realloc overrides only when allowed. On Clang+ASAN we skip
// overrides to avoid AddressSanitizer runtime initialization problems.
#if AERONET_WANT_MALLOC_OVERRIDES
extern "C" void* malloc(size_t size) {
  if (aeronet::test::ShouldFailMalloc()) {
    errno = ENOMEM;
    return nullptr;
  }
  return CallRealMalloc(size);
}

extern "C" void* realloc(void* ptr, size_t size) {
  if (aeronet::test::ShouldFailRealloc()) {
    errno = ENOMEM;
    return nullptr;
  }
  return CallRealRealloc(ptr, size);
}

// free is intentionally left un-overridden.
#endif  // AERONET_WANT_MALLOC_OVERRIDES

namespace aeronet::test {

// RAII guard that forces all malloc/realloc calls to fail while in scope and
// restores normal behavior on destruction.
struct FailAllAllocationsGuard {
  FailAllAllocationsGuard() noexcept {
    FailAllMallocs();
    FailAllReallocs();
  }
  FailAllAllocationsGuard(const FailAllAllocationsGuard&) = delete;
  FailAllAllocationsGuard& operator=(const FailAllAllocationsGuard&) = delete;
  ~FailAllAllocationsGuard() {
    ResetToSysMalloc();
    ResetToSysRealloc();
  }
};

#if AERONET_WANT_SYS_OVERRIDES
inline int CreateMemfd(std::string_view name) {
  const std::string nameStr(name);
  int retfd = static_cast<int>(::syscall(SYS_memfd_create, nameStr.c_str(), MFD_CLOEXEC));
  if (retfd < 0) {
    throw std::runtime_error("memfd_create failed: " + std::string(std::strerror(errno)));
  }
  return retfd;
}
#endif  // AERONET_WANT_SYS_OVERRIDES

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

  [[nodiscard]] std::size_t size(const Key& key) {
    std::scoped_lock<std::mutex> lock(_mutex);
    auto it = _queues.find(key);
    if (it == _queues.end()) {
      return 0;
    }
    return it->second.size();
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

}  // namespace aeronet::test

// Socket syscall action queues: allow tests to simulate syscall errors.
// Action type for socket syscalls: return value (-1 for error) and errno.
using SyscallAction = std::pair<int, int>;  // (return value, errno)

// Simple action type for IO overrides: first = return value (bytes or -1), second = errno to set when returning -1.
using IoAction = std::pair<ssize_t, int>;

namespace aeronet::test {
inline ActionQueue<SyscallAction> g_socket_actions;
inline ActionQueue<SyscallAction> g_setsockopt_actions;
inline ActionQueue<SyscallAction> g_bind_actions;
inline ActionQueue<SyscallAction> g_listen_actions;
inline ActionQueue<SyscallAction> g_accept_actions;
inline ActionQueue<SyscallAction> g_getsockname_actions;
inline ActionQueue<std::pair<ssize_t, int>> g_send_actions;  // (ret, errno)

// Install IO actions on the next accepted client socket.
// This is required because tests create a client-side fd, but the server writes on the
// server-side accepted fd (same process, different fd number).
struct AcceptInstallActions {
  std::vector<IoAction> writeActions;
  std::vector<IoAction> writevActions;
  std::vector<IoAction> sendfileActions;
};

inline ActionQueue<AcceptInstallActions> g_on_accept_install_actions;
inline std::atomic<int> g_last_accepted_fd{-1};
inline std::atomic<std::size_t> g_accept_count{0};

#if AERONET_WANT_SYS_OVERRIDES
// Epoll control action for MOD failures
struct EpollCtlAction {
  int ret{0};  // return value (0 for success, -1 for failure)
  int err{0};  // errno to set on failure
};

inline ActionQueue<EpollCtlAction> g_epoll_ctl_actions;
// Action queue for failing epoll_ctl ADD operations (used to test accept-path error handling)
inline ActionQueue<EpollCtlAction> g_epoll_ctl_add_actions;
// Global flag to fail all epoll_ctl MOD operations for testing error handling
inline std::atomic<bool> g_epoll_ctl_mod_fail{false};
inline int g_epoll_ctl_mod_fail_errno = 0;
// Counter to track how many MOD operations were intercepted (for test validation)
inline std::atomic<std::size_t> g_epoll_ctl_mod_fail_count{0};

// Epoll create action for create failures
struct EpollCreateAction {
  bool fail{false};
  int err{0};
};

inline ActionQueue<EpollCreateAction> g_epoll_create_actions;

// Epoll wait action for wait failures
struct EpollWaitAction {
  enum class Kind : std::uint8_t { Events, Error };
  Kind kind{Kind::Events};
  int result{0};
  int err{0};
  std::vector<struct epoll_event> events;
};

inline ActionQueue<EpollWaitAction> g_epoll_wait_actions;

// Optional default action used when the epoll_wait action queue is exhausted.
// This is primarily to make tests deterministic when the system under test
// calls epoll_wait more times than expected due to timing.
inline std::optional<EpollWaitAction> g_epoll_wait_default_action;

// Helper to fail all epoll_ctl MOD operations with a specific error
inline void FailAllEpollCtlMod(int err) {
  g_epoll_ctl_mod_fail.store(true, std::memory_order_release);
  g_epoll_ctl_mod_fail_errno = err;
  g_epoll_ctl_mod_fail_count.store(0, std::memory_order_release);
}
#endif  // AERONET_WANT_SYS_OVERRIDES

#ifdef AERONET_ENABLE_OPENSSL
// --- OpenSSL kTLS controls (tests only) ---
// Allow tests to inject custom return values for BIO_ctrl when called with
// BIO_CTRL_GET_KTLS_SEND, and to force SSL_get_wbio
// to return nullptr a configurable number of times.

struct BioCtrlAction {
  long ret{0};
  int err{0};
};

// Queue of actions keyed by cmd (e.g., BIO_CTRL_GET_KTLS_SEND)
inline KeyedActionQueue<int, BioCtrlAction> g_bio_ctrl_actions;

// Force N next calls to SSL_get_wbio to return nullptr.
inline std::atomic<int> g_ssl_get_wbio_force_null{0};

inline void ForceNextSslGetWbioNull(int count = 1) {
  g_ssl_get_wbio_force_null.store(count, std::memory_order_release);
}

// Helpers for tests to push actions
inline void PushBioCtrlAction(int cmd, long ret, int err = 0) { g_bio_ctrl_actions.push(cmd, BioCtrlAction{ret, err}); }

#endif  // AERONET_ENABLE_OPENSSL

#ifdef AERONET_ENABLE_OPENSSL
// --- Interposed overrides for OpenSSL functions (tests only) ---
extern "C" long BIO_ctrl(BIO* b, int cmd, long larg, void* parg) {  // NOLINT
  using Fn = long (*)(BIO*, int, long, void*);
  static Fn real_fn = nullptr;
  if (real_fn == nullptr) {
    real_fn = aeronet::test::ResolveNext<Fn>("BIO_ctrl");
  }

#ifdef BIO_CTRL_GET_KTLS_SEND
  if (cmd == BIO_CTRL_GET_KTLS_SEND) {
    if (auto act = aeronet::test::g_bio_ctrl_actions.pop(cmd)) {
      errno = act->err;
      return act->ret;
    }
  }
#endif
  return real_fn(b, cmd, larg, parg);
}

extern "C" BIO* SSL_get_wbio(const SSL* s) {  // NOLINT
  using Fn = BIO* (*)(const SSL*);
  static Fn real_fn = nullptr;
  if (real_fn == nullptr) {
    real_fn = aeronet::test::ResolveNext<Fn>("SSL_get_wbio");
  }
  int remaining = aeronet::test::g_ssl_get_wbio_force_null.load(std::memory_order_acquire);
  while (remaining > 0) {
    if (aeronet::test::g_ssl_get_wbio_force_null.compare_exchange_weak(
            remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return nullptr;
    }
  }
  return real_fn(s);
}
#endif  // AERONET_ENABLE_OPENSSL

#if AERONET_WANT_SYS_OVERRIDES
inline void ResetEpollCtlModFail() {
  g_epoll_ctl_mod_fail.store(false, std::memory_order_release);
  g_epoll_ctl_mod_fail_errno = 0;
  g_epoll_ctl_mod_fail_count.store(0, std::memory_order_release);
}

inline std::size_t GetEpollCtlModFailCount() { return g_epoll_ctl_mod_fail_count.load(std::memory_order_acquire); }
#endif  // AERONET_WANT_SYS_OVERRIDES

inline void ResetSocketActions() {
  g_socket_actions.reset();
  g_setsockopt_actions.reset();
  g_bind_actions.reset();
  g_listen_actions.reset();
  g_accept_actions.reset();
  g_getsockname_actions.reset();
  g_send_actions.reset();
  g_on_accept_install_actions.reset();
  g_last_accepted_fd.store(-1, std::memory_order_release);
  g_accept_count.store(0, std::memory_order_release);
#if AERONET_WANT_SYS_OVERRIDES
  g_epoll_ctl_actions.reset();
  g_epoll_ctl_add_actions.reset();
  g_epoll_create_actions.reset();
  g_epoll_wait_actions.reset();
  ResetEpollCtlModFail();
#endif  // AERONET_WANT_SYS_OVERRIDES
}

inline void PushSocketAction(SyscallAction action) { g_socket_actions.push(action); }
inline void PushSetsockoptAction(SyscallAction action) { g_setsockopt_actions.push(action); }
inline void PushBindAction(SyscallAction action) { g_bind_actions.push(action); }
inline void PushListenAction(SyscallAction action) { g_listen_actions.push(action); }
inline void PushAcceptAction(SyscallAction action) { g_accept_actions.push(action); }
inline void PushGetsocknameAction(SyscallAction action) { g_getsockname_actions.push(action); }
inline void PushSendAction(std::pair<ssize_t, int> action) { g_send_actions.push(action); }
#if AERONET_WANT_SYS_OVERRIDES
inline void PushEpollCtlAction(EpollCtlAction action) { g_epoll_ctl_actions.push(action); }
inline void PushEpollCtlAddAction(EpollCtlAction action) { g_epoll_ctl_add_actions.push(action); }
inline void PushEpollCreateAction(EpollCreateAction action) { g_epoll_create_actions.push(action); }
inline void PushEpollWaitAction(EpollWaitAction action) { g_epoll_wait_actions.push(std::move(action)); }
#endif  // AERONET_WANT_SYS_OVERRIDES

using SocketFn = int (*)(int, int, int);
using SetsockoptFn = int (*)(int, int, int, const void*, socklen_t);
using BindFn = int (*)(int, const struct sockaddr*, socklen_t);
using ListenFn = int (*)(int, int);
using AcceptFn = int (*)(int, struct sockaddr*, socklen_t*);
using GetsocknameFn = int (*)(int, struct sockaddr*, socklen_t*);
using SendFn = ssize_t (*)(int, const void*, size_t, int);
#if AERONET_WANT_SYS_OVERRIDES
using Accept4Fn = int (*)(int, struct sockaddr*, socklen_t*, int);
using EpollCtlFn = int (*)(int, int, int, struct epoll_event*);
using EpollCreateFn = int (*)(int);
using EpollWaitFn = int (*)(int, struct epoll_event*, int, int);
#endif  // AERONET_WANT_SYS_OVERRIDES

inline SocketFn ResolveRealSocket() {
  static SocketFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<SocketFn>("socket");
  return fn;
}

inline SetsockoptFn ResolveRealSetsockopt() {
  static SetsockoptFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<SetsockoptFn>("setsockopt");
  return fn;
}

inline BindFn ResolveRealBind() {
  static BindFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<BindFn>("bind");
  return fn;
}

inline ListenFn ResolveRealListen() {
  static ListenFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<ListenFn>("listen");
  return fn;
}

inline AcceptFn ResolveRealAccept() {
  static AcceptFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<AcceptFn>("accept");
  return fn;
}

#if AERONET_WANT_SYS_OVERRIDES
inline Accept4Fn ResolveRealAccept4() {
  static Accept4Fn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<Accept4Fn>("accept4");
  return fn;
}
#endif  // AERONET_WANT_SYS_OVERRIDES

inline GetsocknameFn ResolveRealGetsockname() {
  static GetsocknameFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<GetsocknameFn>("getsockname");
  return fn;
}

inline SendFn ResolveRealSend() {
  static SendFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<SendFn>("send");
  return fn;
}

#if AERONET_WANT_SYS_OVERRIDES
inline EpollCtlFn ResolveRealEpollCtl() {
  static EpollCtlFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<EpollCtlFn>("epoll_ctl");
  return fn;
}

inline EpollCreateFn ResolveRealEpollCreate1() {
  static EpollCreateFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<EpollCreateFn>("epoll_create1");
  return fn;
}

inline EpollWaitFn ResolveRealEpollWait() {
  static EpollWaitFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<EpollWaitFn>("epoll_wait");
  return fn;
}

inline void ResetEpollHooks() {
  test::g_epoll_create_actions.reset();
  test::g_epoll_wait_actions.reset();
  test::g_epoll_wait_default_action.reset();
  test::ResetEpollCtlModFail();
  test::FailNextMalloc(0);
  test::FailNextRealloc(0);
}

inline void SetEpollCreateActions(std::initializer_list<EpollCreateAction> actions) {
  test::g_epoll_create_actions.setActions(actions);
}

inline void SetEpollWaitActions(std::vector<EpollWaitAction> actions) {
  test::g_epoll_wait_default_action.reset();
  if (!actions.empty()) {
    // Repeat the last action if the queue is exhausted.
    test::g_epoll_wait_default_action = actions.back();
  }
  test::g_epoll_wait_actions.setActions(std::move(actions));
}

struct EventLoopHookGuard {
  EventLoopHookGuard() = default;

  EventLoopHookGuard(const EventLoopHookGuard&) = delete;
  EventLoopHookGuard(EventLoopHookGuard&&) = delete;
  EventLoopHookGuard& operator=(const EventLoopHookGuard&) = delete;
  EventLoopHookGuard& operator=(EventLoopHookGuard&&) = delete;

  ~EventLoopHookGuard() { ResetEpollHooks(); }
};

[[nodiscard]] inline EpollCreateAction EpollCreateFail(int err) { return EpollCreateAction{true, err}; }

[[nodiscard]] inline EpollWaitAction WaitReturn(int readyCount, std::vector<epoll_event> events) {
  EpollWaitAction action;
  action.kind = EpollWaitAction::Kind::Events;
  action.result = readyCount;
  action.events = std::move(events);
  return action;
}

[[nodiscard]] inline EpollWaitAction WaitError(int err) {
  EpollWaitAction action;
  action.kind = EpollWaitAction::Kind::Error;
  action.err = err;
  return action;
}

epoll_event inline MakeEvent(int fd, uint32_t mask) {
  epoll_event ev{};
  ev.events = mask;
  ev.data.fd = fd;
  return ev;
}
#endif  // AERONET_WANT_SYS_OVERRIDES

}  // namespace aeronet::test

// IO override control: enable/disable replacement of ::read/::write for tests.

namespace aeronet::test {
inline KeyedActionQueue<int, IoAction> g_read_actions;
inline KeyedActionQueue<int, IoAction> g_write_actions;

// connect override action queue: return (0 for success) or (-1, errno)
inline ActionQueue<std::pair<int, int>> g_connect_actions;

inline void PushConnectAction(std::pair<int, int> action) { g_connect_actions.push(action); }

using ConnectFn = int (*)(int, const struct sockaddr*, socklen_t);

inline ConnectFn ResolveRealConnect() {
  static ConnectFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<ConnectFn>("connect");
  return fn;
}

inline void ResetIoActions() {
  g_read_actions.reset();
  g_write_actions.reset();
}

inline void SetReadActions(int fd, std::initializer_list<IoAction> actions) { g_read_actions.setActions(fd, actions); }
inline void SetWriteActions(int fd, std::initializer_list<IoAction> actions) {
  g_write_actions.setActions(fd, actions);
}
inline void PushReadAction(int fd, IoAction action) { g_read_actions.push(fd, action); }
inline void PushWriteAction(int fd, IoAction action) { g_write_actions.push(fd, action); }

using ReadFn = ssize_t (*)(int, void*, size_t);
using WriteFn = ssize_t (*)(int, const void*, size_t);
using WritevFn = ssize_t (*)(int, const struct iovec*, int);
using SendmsgFn = ssize_t (*)(int, const struct msghdr*, int);

inline KeyedActionQueue<int, IoAction> g_writev_actions;
inline KeyedActionQueue<int, IoAction> g_sendmsg_actions;

inline ReadFn ResolveRealRead() {
  static ReadFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<ReadFn>("read");
  return fn;
}

inline WriteFn ResolveRealWrite() {
  static WriteFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<WriteFn>("write");
  return fn;
}

inline WritevFn ResolveRealWritev() {
  static WritevFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<WritevFn>("writev");
  return fn;
}

inline SendmsgFn ResolveRealSendmsg() {
  static SendmsgFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<SendmsgFn>("sendmsg");
  return fn;
}

inline void SetWritevActions(int fd, std::initializer_list<IoAction> actions) {
  g_writev_actions.setActions(fd, actions);
}

inline void SetSendmsgActions(int fd, std::initializer_list<IoAction> actions) {
  g_sendmsg_actions.setActions(fd, actions);
}

inline void PushSendmsgAction(int fd, IoAction action) { g_sendmsg_actions.push(fd, action); }

}  // namespace aeronet::test

#ifdef AERONET_WANT_SENDFILE_PREAD_OVERRIDES

// Minimal overrides for sendfile(2) and pread(2) to simulate errno paths in tests
namespace aeronet::test {
inline std::optional<std::string> PathForFd(int fd) {
  std::array<char, 64> linkBuf{};
  std::snprintf(linkBuf.data(), linkBuf.size(), "/proc/self/fd/%d", fd);
  std::array<char, 512> pathBuf{};
  const auto len = ::readlink(linkBuf.data(), pathBuf.data(), pathBuf.size() - 1);
  if (len <= 0) {
    return std::nullopt;
  }
  pathBuf[static_cast<std::size_t>(len)] = '\0';
  return std::string(pathBuf.data());
}
using PreadFn = ssize_t (*)(int, void*, size_t, off_t);
using SendfileFn = ssize_t (*)(int, int, off_t*, size_t);

inline PreadFn ResolveRealPread() {
  static PreadFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<PreadFn>("pread");
  return fn;
}

inline SendfileFn ResolveRealSendfile() {
  static SendfileFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<SendfileFn>("sendfile");
  return fn;
}

inline KeyedActionQueue<int, IoAction> g_pread_actions;
inline KeyedActionQueue<int, IoAction> g_sendfile_actions;            // keyed by out_fd (destination)
inline KeyedActionQueue<std::string, IoAction> g_pread_path_actions;  // keyed by file path

inline void ResetPreadSendfile() {
  g_pread_actions.reset();
  g_sendfile_actions.reset();
  g_pread_path_actions.reset();
}

inline void SetPreadActions(int fd, std::initializer_list<IoAction> actions) {
  g_pread_actions.setActions(fd, actions);
}

inline void SetSendfileActions(int outFd, std::initializer_list<IoAction> actions) {
  g_sendfile_actions.setActions(outFd, actions);
}

inline void SetPreadPathActions(std::string_view path, std::initializer_list<IoAction> actions) {
  g_pread_path_actions.setActions(std::string(path), actions);
}
}  // namespace aeronet::test

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  if (auto act = aeronet::test::g_pread_actions.pop(fd)) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      // Real pread(2) never returns more than 'count'. Clamp to avoid UB in callers.
      ret = std::min<ssize_t>(ret, static_cast<ssize_t>(count));
      if (buf != nullptr && ret > 0) {
        std::memset(buf, 'B', static_cast<size_t>(std::min<ssize_t>(ret, static_cast<ssize_t>(count))));
      }
      return ret;
    }
    errno = err;
    return -1;
  }
  // Try path-based actions by resolving fd -> path
  if (auto pathOpt = aeronet::test::PathForFd(fd)) {
    if (auto pact = aeronet::test::g_pread_path_actions.pop(*pathOpt)) {
      auto [ret, err] = *pact;
      if (ret >= 0) {
        if (buf != nullptr && ret > 0) {
          std::memset(buf, 'B', static_cast<size_t>(std::min<ssize_t>(ret, static_cast<ssize_t>(count))));
        }
        return ret;
      }
      errno = err;
      return -1;
    }
  }
  auto real = aeronet::test::ResolveRealPread();
  return real(fd, buf, count, offset);
}

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) ssize_t sendfile(int out_fd, int in_fd, off_t* offset,
                                                                    size_t count) {
  if (auto act = aeronet::test::g_sendfile_actions.pop(out_fd)) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      // Real sendfile(2) never returns more than 'count'. Clamp for caller invariants.
      ret = std::min<ssize_t>(ret, static_cast<ssize_t>(count));
      // pretend we sent ret bytes by advancing offset if provided
      if (offset != nullptr) {
        *offset += ret;
      }
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealSendfile();
  return real(out_fd, in_fd, offset, count);
}

#endif  // AERONET_WANT_SENDFILE_PREAD_OVERRIDES

#ifdef AERONET_WANT_READ_WRITE_OVERRIDES

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) ssize_t read(int fd, void* buf, size_t count) {
  auto act = aeronet::test::g_read_actions.pop(fd);
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      // Real read(2) never returns more than 'count'. Clamp to avoid corrupting buffers
      // when tests enqueue an oversized action.
      ret = std::min<ssize_t>(ret, static_cast<ssize_t>(count));
      if ((buf != nullptr) && ret > 0) {
        size_t fill = static_cast<size_t>(ret);
        std::memset(buf, 'A', fill);
      }
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealRead();
  return real(fd, buf, count);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" __attribute__((no_sanitize("address"))) ssize_t write(int fd, const void* buf, size_t count) {
  auto act = aeronet::test::g_write_actions.pop(fd);
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      // Real write(2) never returns more than 'count'. Clamp for caller invariants.
      ret = std::min<ssize_t>(ret, static_cast<ssize_t>(count));
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealWrite();
  return real(fd, buf, count);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" __attribute__((no_sanitize("address"))) ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
  auto act = aeronet::test::g_writev_actions.pop(fd);
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      // Real writev(2) never returns more than the sum of iov lengths.
      std::size_t total = 0;
      for (int idx = 0; idx < iovcnt; ++idx) {
        total += iov[idx].iov_len;
      }
      ret = std::min<ssize_t>(ret, static_cast<ssize_t>(total));
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealWritev();
  return real(fd, iov, iovcnt);
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
extern "C" __attribute__((no_sanitize("address"))) ssize_t sendmsg(int fd, const struct msghdr* msg, int flags) {
  auto act = aeronet::test::g_sendmsg_actions.pop(fd);
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      // Real sendmsg(2) never returns more than the sum of iov lengths.
      std::size_t total = 0;
      for (std::size_t idx = 0; std::cmp_less(idx, msg->msg_iovlen); ++idx) {
        total += msg->msg_iov[idx].iov_len;
      }
      ret = std::min<ssize_t>(ret, static_cast<ssize_t>(total));
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealSendmsg();
  return real(fd, msg, flags);
}

#endif

#ifdef AERONET_WANT_SOCKET_OVERRIDES

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int socket(int domain, int type, int protocol) {
  auto act = aeronet::test::g_socket_actions.pop();
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealSocket();
  return real(domain, type, protocol);
}

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int setsockopt(int sockfd, int level, int optname,
                                                                  const void* optval, socklen_t optlen) {
  auto act = aeronet::test::g_setsockopt_actions.pop();
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealSetsockopt();
  return real(sockfd, level, optname, optval, optlen);
}

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int bind(int sockfd, const struct sockaddr* addr,
                                                            socklen_t addrlen) {
  auto act = aeronet::test::g_bind_actions.pop();
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealBind();
  return real(sockfd, addr, addrlen);
}

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int listen(int sockfd, int backlog) {
  auto act = aeronet::test::g_listen_actions.pop();
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealListen();
  return real(sockfd, backlog);
}

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
  auto act = aeronet::test::g_accept_actions.pop();
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      return ret;
    }
    errno = err;
    return -1;
  }

  auto real = aeronet::test::ResolveRealAccept();
  const int fd = real(sockfd, addr, addrlen);
  if (fd >= 0) {
    aeronet::test::g_last_accepted_fd.store(fd, std::memory_order_release);
    aeronet::test::g_accept_count.fetch_add(1, std::memory_order_acq_rel);
    if (auto install = aeronet::test::g_on_accept_install_actions.pop()) {
      if (!install->writeActions.empty()) {
        aeronet::test::g_write_actions.setActions(fd, install->writeActions);
      }
      if (!install->writevActions.empty()) {
        aeronet::test::g_writev_actions.setActions(fd, install->writevActions);
      }
#ifdef AERONET_WANT_SENDFILE_PREAD_OVERRIDES
      if (!install->sendfileActions.empty()) {
        aeronet::test::g_sendfile_actions.setActions(fd, install->sendfileActions);
      }
#endif
    }
  }
  return fd;
}

#if AERONET_WANT_SYS_OVERRIDES
// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int accept4(int sockfd, struct sockaddr* addr, socklen_t* addrlen,
                                                               int flags) {
  auto act = aeronet::test::g_accept_actions.pop();
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      return ret;
    }
    errno = err;
    return -1;
  }

  auto real = aeronet::test::ResolveRealAccept4();
  const int fd = real(sockfd, addr, addrlen, flags);
  if (fd >= 0) {
    aeronet::test::g_last_accepted_fd.store(fd, std::memory_order_release);
    aeronet::test::g_accept_count.fetch_add(1, std::memory_order_acq_rel);
    if (auto install = aeronet::test::g_on_accept_install_actions.pop()) {
      if (!install->writeActions.empty()) {
        aeronet::test::g_write_actions.setActions(fd, install->writeActions);
      }
      if (!install->writevActions.empty()) {
        aeronet::test::g_writev_actions.setActions(fd, install->writevActions);
      }
#ifdef AERONET_WANT_SENDFILE_PREAD_OVERRIDES
      if (!install->sendfileActions.empty()) {
        aeronet::test::g_sendfile_actions.setActions(fd, install->sendfileActions);
      }
#endif
    }
  }
  return fd;
}
#endif  // AERONET_WANT_SYS_OVERRIDES

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int connect(int sockfd, const struct sockaddr* addr,
                                                               socklen_t addrlen) {
  auto act = aeronet::test::g_connect_actions.pop();
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealConnect();
  return real(sockfd, addr, addrlen);
}

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int getsockname(int sockfd, struct sockaddr* addr,
                                                                   socklen_t* addrlen) {
  auto act = aeronet::test::g_getsockname_actions.pop();
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealGetsockname();
  return real(sockfd, addr, addrlen);
}

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) ssize_t send(int sockfd, const void* buf, size_t len, int flags) {
  auto act = aeronet::test::g_send_actions.pop();
  if (act) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealSend();
  return real(sockfd, buf, len, flags);
}

#if AERONET_WANT_SYS_OVERRIDES
// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
  if (op == EPOLL_CTL_ADD) {
    // Allow tests to fail a specific ADD operation without impacting unrelated setup/teardown.
    auto act = aeronet::test::g_epoll_ctl_add_actions.pop();
    if (act && act->ret != 0) {
      errno = act->err;
      return -1;
    }
  }
  // Only inject failures for MOD operations to allow normal ADD/DEL for setup/teardown
  if (op == EPOLL_CTL_MOD) {
    // Check global persistent fail flag first
    if (aeronet::test::g_epoll_ctl_mod_fail.load(std::memory_order_acquire)) {
      aeronet::test::g_epoll_ctl_mod_fail_count.fetch_add(1, std::memory_order_relaxed);
      errno = aeronet::test::g_epoll_ctl_mod_fail_errno;
      return -1;
    }
    // Otherwise check action queue for per-call failures
    auto act = aeronet::test::g_epoll_ctl_actions.pop();
    if (act && act->ret != 0) {
      errno = act->err;
      return -1;
    }
  }
  auto real = aeronet::test::ResolveRealEpollCtl();
  return real(epfd, op, fd, event);
}

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int epoll_create1(int flags) {
  auto action = aeronet::test::g_epoll_create_actions.pop();
  if (action && action->fail) {
    errno = action->err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealEpollCreate1();
  return real(flags);
}

// NOLINTNEXTLINE
extern "C" __attribute__((no_sanitize("address"))) int epoll_wait(int epfd, struct epoll_event* events, int maxevents,
                                                                  int timeout) {
  auto action = aeronet::test::g_epoll_wait_actions.pop();
  const auto applyAction = [&](const aeronet::test::EpollWaitAction& act) -> int {
    if (act.kind == aeronet::test::EpollWaitAction::Kind::Error) {
      errno = act.err;
      return -1;
    }
    const std::size_t limit = std::min(static_cast<std::size_t>(act.result), static_cast<std::size_t>(maxevents));
    for (std::size_t i = 0; i < limit && i < act.events.size(); ++i) {
      events[i] = act.events[i];
    }
    return act.result;
  };

  if (action) {
    return applyAction(*action);
  }
  if (aeronet::test::g_epoll_wait_default_action) {
    return applyAction(*aeronet::test::g_epoll_wait_default_action);
  }
  auto real = aeronet::test::ResolveRealEpollWait();
  return real(epfd, events, maxevents, timeout);
}

namespace aeronet::test {
inline KeyedActionQueue<int, IoAction> g_recvmsg_actions;
}

namespace aeronet::test {
inline KeyedActionQueue<int, int> g_recvmsg_modes;
}

extern "C" __attribute__((no_sanitize("address"))) ssize_t recvmsg(int fd, struct msghdr* msg, int flags) {
  using RecvmsgFn = ssize_t (*)(int, struct msghdr*, int);
  if (auto act = ::aeronet::test::g_recvmsg_actions.pop(fd)) {
    auto [ret, err] = *act;
    if (ret >= 0) {
      if (msg != nullptr && msg->msg_control != nullptr &&
          msg->msg_controllen >= static_cast<socklen_t>(CMSG_LEN(sizeof(struct sock_extended_err)))) {
        std::optional<int> modeOpt = ::aeronet::test::g_recvmsg_modes.pop(fd);
        if (modeOpt && *modeOpt == 8) {
          // Tests requested no control message: leave msg_controllen 0
          if (msg) msg->msg_controllen = 0;
        } else {
          auto* c = reinterpret_cast<struct cmsghdr*>(msg->msg_control);
          c->cmsg_len = CMSG_LEN(sizeof(struct sock_extended_err));
          // default to IPv4 errqueue entry
          c->cmsg_level = SOL_IP;
          c->cmsg_type = IP_RECVERR;
          if (modeOpt && *modeOpt == 6) {
            c->cmsg_level = SOL_IPV6;
            c->cmsg_type = IPV6_RECVERR;
          } else if (modeOpt && *modeOpt == 7) {
            // synthesize an unknown control message (not IP_RECVERR)
            c->cmsg_level = SOL_IP;
            c->cmsg_type = 0;
          } else if (modeOpt && *modeOpt == 9) {
            // synthesize an IPv6 control message with wrong type (not IPV6_RECVERR)
            c->cmsg_level = SOL_IPV6;
            c->cmsg_type = 0;
          }

          auto* serr = reinterpret_cast<struct sock_extended_err*>(CMSG_DATA(c));
          std::memset(serr, 0, sizeof(*serr));
          // allow tests to synthesize non-zerocopy origins
          if (modeOpt && *modeOpt == 2) {
            serr->ee_origin = 0;  // not SO_EE_ORIGIN_ZEROCOPY
          } else {
            serr->ee_origin = SO_EE_ORIGIN_ZEROCOPY;
          }
          serr->ee_data = 42;
          msg->msg_controllen = c->cmsg_len;
        }
      }
      return ret;
    }
    errno = err;
    return -1;
  }
  static RecvmsgFn real = nullptr;
  if (real == nullptr) {
    real = ::aeronet::test::ResolveNext<RecvmsgFn>("recvmsg");
  }
  return real(fd, msg, flags);
}
#endif  // AERONET_WANT_SYS_OVERRIDES

#endif