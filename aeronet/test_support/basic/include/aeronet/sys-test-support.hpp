#pragma once

#include <dlfcn.h>
#include <linux/memfd.h>
#include <sys/socket.h>
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

}  // namespace aeronet::test

// Socket syscall action queues: allow tests to simulate syscall errors.
// Action type for socket syscalls: return value (-1 for error) and errno.
using SyscallAction = std::pair<int, int>;  // (return value, errno)

namespace aeronet::test {
inline ActionQueue<SyscallAction> g_socket_actions;
inline ActionQueue<SyscallAction> g_setsockopt_actions;
inline ActionQueue<SyscallAction> g_bind_actions;
inline ActionQueue<SyscallAction> g_listen_actions;
inline ActionQueue<SyscallAction> g_getsockname_actions;

inline void ResetSocketActions() {
  g_socket_actions.reset();
  g_setsockopt_actions.reset();
  g_bind_actions.reset();
  g_listen_actions.reset();
  g_getsockname_actions.reset();
}

inline void PushSocketAction(SyscallAction action) { g_socket_actions.push(action); }
inline void PushSetsockoptAction(SyscallAction action) { g_setsockopt_actions.push(action); }
inline void PushBindAction(SyscallAction action) { g_bind_actions.push(action); }
inline void PushListenAction(SyscallAction action) { g_listen_actions.push(action); }
inline void PushGetsocknameAction(SyscallAction action) { g_getsockname_actions.push(action); }

using SocketFn = int (*)(int, int, int);
using SetsockoptFn = int (*)(int, int, int, const void*, socklen_t);
using BindFn = int (*)(int, const struct sockaddr*, socklen_t);
using ListenFn = int (*)(int, int);
using GetsocknameFn = int (*)(int, struct sockaddr*, socklen_t*);

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

inline GetsocknameFn ResolveRealGetsockname() {
  static GetsocknameFn fn = nullptr;
  if (fn != nullptr) {
    return fn;
  }
  fn = aeronet::test::ResolveNext<GetsocknameFn>("getsockname");
  return fn;
}

}  // namespace aeronet::test

// IO override control: enable/disable replacement of ::read/::write for tests.

// Simple action type: first = return value (bytes or -1), second = errno to set when returning -1
using IoAction = std::pair<ssize_t, int>;

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
      if ((buf != nullptr) && ret > 0) {
        size_t fill = static_cast<size_t>(std::min<ssize_t>(ret, static_cast<ssize_t>(count)));
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
      // pretend we wrote ret bytes
      return ret;
    }
    errno = err;
    return -1;
  }
  auto real = aeronet::test::ResolveRealWrite();
  return real(fd, buf, count);
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

#endif