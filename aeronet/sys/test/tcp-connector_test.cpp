#include "aeronet/tcp-connector.hpp"

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/vector.hpp"

using namespace aeronet;

namespace {

struct ConnectAction {
  enum class Kind : std::uint8_t { Real, Error, Success };
  Kind kind{Kind::Real};
  int err{0};
};

[[nodiscard]] ConnectAction ConnectErr(int err) { return ConnectAction{ConnectAction::Kind::Error, err}; }
[[nodiscard]] ConnectAction ConnectSuccess() { return ConnectAction{ConnectAction::Kind::Success, 0}; }

struct PollAction {
  enum class Kind : std::uint8_t { Real, Return };
  Kind kind{Kind::Real};
  int result{0};                             // poll() return value when kind == Return
  int err{0};                                // errno to set when result < 0
  std::chrono::milliseconds sleepBefore{0};  // simulate poll() blocking before it returns
};

[[nodiscard]] PollAction PollErr(int err, std::chrono::milliseconds sleepBefore = std::chrono::milliseconds(0)) {
  return PollAction{PollAction::Kind::Return, -1, err, sleepBefore};
}
[[nodiscard]] PollAction PollTimeout(std::chrono::milliseconds sleepBefore = std::chrono::milliseconds(0)) {
  return PollAction{PollAction::Kind::Return, 0, 0, sleepBefore};
}

struct TestAddrEntry {
  sockaddr_storage storage{};
  socklen_t addrlen{0};
  int family{AF_INET};
  int sockType{SOCK_STREAM};
  int protocol{0};
};

[[nodiscard]] TestAddrEntry MakeLoopbackEntry(uint16_t port) {
  TestAddrEntry entry;
  auto* sin = reinterpret_cast<sockaddr_in*>(&entry.storage);
  sin->sin_family = AF_INET;
  sin->sin_port = htons(port);
  sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  entry.addrlen = sizeof(sockaddr_in);
  entry.family = AF_INET;
  entry.sockType = SOCK_STREAM;
  entry.protocol = IPPROTO_TCP;
  return entry;
}

// IPv6 loopback (::1) candidate, mirroring how getaddrinfo("localhost") often yields ::1 before 127.0.0.1.
[[nodiscard]] TestAddrEntry MakeLoopback6Entry(uint16_t port) {
  TestAddrEntry entry;
  auto* sin6 = reinterpret_cast<sockaddr_in6*>(&entry.storage);
  sin6->sin6_family = AF_INET6;
  sin6->sin6_port = htons(port);
  sin6->sin6_addr = in6addr_loopback;  // ::1
  entry.addrlen = sizeof(sockaddr_in6);
  entry.family = AF_INET6;
  entry.sockType = SOCK_STREAM;
  entry.protocol = IPPROTO_TCP;
  return entry;
}

// Bind an ephemeral IPv4 loopback port, then release it: returns a port number on which nothing listens,
// so a subsequent connect is refused. (Real connect()/poll(), not interposed, exercise the fallback path.)
[[nodiscard]] uint16_t ReserveClosedLoopbackPort() {
  const BaseFd probe(::socket(AF_INET, SOCK_STREAM, 0));
  assert(probe.fd() >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  [[maybe_unused]] const int bindRc = ::bind(probe.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  assert(bindRc == 0);
  socklen_t len = sizeof(addr);
  [[maybe_unused]] const int nameRc = ::getsockname(probe.fd(), reinterpret_cast<sockaddr*>(&addr), &len);
  assert(nameRc == 0);
  return ntohs(addr.sin_port);
}

struct AddrinfoOverrideData {
  int result{0};
  vector<TestAddrEntry> entries;
};

struct TestAddrinfoNode {
  addrinfo ai{};
  sockaddr_storage storage{};
};

// --- Mock-harness hardening -------------------------------------------------------------
//
// socket()/connect()/poll() below are process-wide interposition: *any* code in this binary
// that touches these libc functions goes through them, not just the calls ConnectTCP() makes
// for the candidate a given test is driving.
struct HookState {
  HookState() noexcept = default;

  std::deque<int> socketErrnos;
  std::deque<ConnectAction> connectActions;
  std::deque<PollAction> pollActions;
  std::optional<AddrinfoOverrideData> addrinfoOverride;
  std::unordered_set<addrinfo*> customHeads;

  // fd most recently returned by our own socket() hook, on this thread.
  int lastOwnedFd{-1};

  // Armed by Set*ActionSequence() whenever given a non-empty sequence; see block comment above.
  bool connectActionsArmed{false};
  bool pollActionsArmed{false};
};

HookState gHookState;

[[nodiscard]] TestAddrinfoNode* DuplicateEntries(const vector<TestAddrEntry>& entries) {
  TestAddrinfoNode* head = nullptr;
  TestAddrinfoNode* tail = nullptr;
  for (const auto& entry : entries) {
    auto* node = new TestAddrinfoNode();
    node->ai.ai_family = entry.family;
    node->ai.ai_socktype = entry.sockType;
    node->ai.ai_protocol = entry.protocol;
    node->ai.ai_addrlen = entry.addrlen;
    node->storage = entry.storage;
    node->ai.ai_addr = reinterpret_cast<sockaddr*>(&node->storage);
    node->ai.ai_next = nullptr;
    if (tail != nullptr) {
      tail->ai.ai_next = &node->ai;
    } else {
      head = node;
    }
    tail = node;
  }
  return head;
}

void FreeCustomList(addrinfo* head) {
  auto* node = reinterpret_cast<TestAddrinfoNode*>(head);
  while (node != nullptr) {
    auto* next = node->ai.ai_next != nullptr ? reinterpret_cast<TestAddrinfoNode*>(node->ai.ai_next) : nullptr;
    delete node;
    node = next;
  }
}

void ResetHooks() {
  gHookState.socketErrnos.clear();
  gHookState.connectActions.clear();
  gHookState.pollActions.clear();
  gHookState.addrinfoOverride.reset();
  for (addrinfo* head : gHookState.customHeads) {
    FreeCustomList(head);
  }
  gHookState.customHeads.clear();
  gHookState.lastOwnedFd = -1;
  gHookState.connectActionsArmed = false;
  gHookState.pollActionsArmed = false;
}

class HookGuard {
 public:
  HookGuard() = default;

  HookGuard(HookGuard&&) = delete;
  HookGuard(const HookGuard&) = delete;
  HookGuard& operator=(const HookGuard&) = delete;
  HookGuard& operator=(HookGuard&&) = delete;

  ~HookGuard() { ResetHooks(); }
};

void SetSocketErrorSequence(std::initializer_list<int> errs) {
  gHookState.socketErrnos.assign(errs.begin(), errs.end());
}

void SetConnectActionSequence(std::initializer_list<ConnectAction> actions) {
  gHookState.connectActions.assign(actions.begin(), actions.end());
  gHookState.connectActionsArmed = actions.size() > 0;
}

void SetPollActionSequence(std::initializer_list<PollAction> actions) {
  gHookState.pollActions.assign(actions.begin(), actions.end());
  gHookState.pollActionsArmed = actions.size() > 0;
}

class AddrinfoOverrideGuard {
 public:
  AddrinfoOverrideGuard() = default;

  explicit AddrinfoOverrideGuard(vector<TestAddrEntry> entries, int result = 0) {
    activate(std::move(entries), result);
  }

  AddrinfoOverrideGuard(const AddrinfoOverrideGuard&) = delete;
  AddrinfoOverrideGuard& operator=(const AddrinfoOverrideGuard&) = delete;

  AddrinfoOverrideGuard(AddrinfoOverrideGuard&& other) noexcept { active_ = std::exchange(other.active_, false); }

  AddrinfoOverrideGuard& operator=(AddrinfoOverrideGuard&& other) noexcept {
    if (this != &other) {
      reset();
      active_ = std::exchange(other.active_, false);
    }
    return *this;
  }

  ~AddrinfoOverrideGuard() { reset(); }

  static AddrinfoOverrideGuard WithError(int result) { return AddrinfoOverrideGuard({}, result); }

  void reset() {
    if (!active_) {
      return;
    }
    gHookState.addrinfoOverride.reset();
    active_ = false;
  }

 private:
  bool active_{false};

  void activate(vector<TestAddrEntry> entries, int result) {
    gHookState.addrinfoOverride = AddrinfoOverrideData{result, std::move(entries)};
    active_ = true;
  }
};

struct HostPortBuffer {
  aeronet::RawChars storage;
  std::span<char> host;
  uint16_t port{};
};

[[nodiscard]] HostPortBuffer MakeHostPortBuffer(std::string_view host, uint16_t port) {
  HostPortBuffer buffer;
  // getaddrinfo (via ConnectTCP) needs a writable host buffer with one spare byte at the end.
  buffer.storage.reserve(host.size() + 1);
  buffer.storage.unchecked_append(host);
  buffer.storage.unchecked_push_back('\0');
  buffer.host = std::span<char>(buffer.storage.data(), host.size());
  buffer.port = port;
  return buffer;
}

}  // namespace

extern "C" {

int socket(int _domain, int _type, int _protocol) {
  using SocketFn = int (*)(int, int, int);
  static SocketFn real_socket = reinterpret_cast<SocketFn>(dlsym(RTLD_NEXT, "socket"));
  if (real_socket == nullptr) {
    std::abort();
  }
  int err = 0;
  if (!gHookState.socketErrnos.empty()) {
    err = gHookState.socketErrnos.front();
    gHookState.socketErrnos.pop_front();
  }
  if (err != 0) {
    errno = err;
    return -1;
  }
  const int fd = real_socket(_domain, _type, _protocol);
  if (fd >= 0) {
    // Remember the fd we just handed out, on this thread: connect()/poll() below only ever
    // consume a queued action for *this* fd, on *this* thread's state, so a socket created
    // anywhere else (a different thread entirely, thanks to thread_local) can never steal or
    // clobber tracking for the candidate ConnectTCP() is currently driving here.
    gHookState.lastOwnedFd = fd;
  }
  return fd;
}

int connect(int _fd, const sockaddr* _addr, socklen_t _len) {
  using ConnectFn = int (*)(int, const sockaddr*, socklen_t);
  static ConnectFn real_connect = reinterpret_cast<ConnectFn>(dlsym(RTLD_NEXT, "connect"));
  if (real_connect == nullptr) {
    std::abort();
  }
  ConnectAction action;
  bool hasAction = false;
  bool armedButEmpty = false;
  if (_fd == gHookState.lastOwnedFd) {
    if (!gHookState.connectActions.empty()) {
      action = gHookState.connectActions.front();
      gHookState.connectActions.pop_front();
      hasAction = true;
    } else if (gHookState.connectActionsArmed) {
      armedButEmpty = true;
    }
  }
  if (armedButEmpty) {
    ADD_FAILURE() << "connect() invoked on fd " << _fd
                  << " after the mocked connect-action queue was already exhausted. Either "
                     "ConnectTCP() made more connect() calls than this test armed, or "
                     "connect() interposition was bypassed for this call.";
  }
  if (hasAction) {
    if (action.kind == ConnectAction::Kind::Error) {
      errno = action.err;
      return -1;
    }
    if (action.kind == ConnectAction::Kind::Success) {
      errno = 0;
      return 0;
    }
  }
  return real_connect(_fd, _addr, _len);
}

// NOLINTNEXTLINE(misc-include-cleaner)
int poll(struct pollfd* _fds, nfds_t _nfds, int _timeout) {
  using PollFn = int (*)(struct pollfd*, nfds_t, int);
  static PollFn real_poll = reinterpret_cast<PollFn>(dlsym(RTLD_NEXT, "poll"));
  if (real_poll == nullptr) {
    std::abort();
  }
  const bool watchesAFd = _nfds > 0 && _fds != nullptr;
  PollAction action;
  bool hasAction = false;
  if (watchesAFd && _fds[0].fd == gHookState.lastOwnedFd) {
    if (!gHookState.pollActions.empty()) {
      action = gHookState.pollActions.front();
      gHookState.pollActions.pop_front();
      hasAction = true;
    } else if (gHookState.pollActionsArmed) {
      ADD_FAILURE() << "poll() invoked on fd " << _fds[0].fd
                    << " after the mocked poll-action queue was already exhausted. This is the "
                       "failure mode behind the earlier flaky CI runs: WaitForConnectCompletion's "
                       "poll() call fell through to the real syscall on a socket that was faked "
                       "into 'connecting' but never really was - and Linux reports such an "
                       "unconnected, non-blocking socket as immediately writable with "
                       "SO_ERROR == 0, which looks exactly like a successful connection.";
    }
  }
  if (hasAction) {
    log::info("Handling poll action for fd {}", _fds[0].fd);
    if (action.sleepBefore.count() > 0) {
      std::this_thread::sleep_for(action.sleepBefore);
    }
    if (action.kind == PollAction::Kind::Return) {
      if (action.result < 0) {
        errno = action.err;
      }
      return action.result;
    }
  }
  log::error("Falling through to real poll syscall for fd {}", _fds[0].fd);
  return real_poll(_fds, _nfds, _timeout);
}

#ifdef __GLIBC__
// Ubuntu's gcc packages enable _FORTIFY_SOURCE by default whenever any optimization is requested (-O1+), even without
// the build asking for it. That can turn a `poll(fds, nfds, timeout)` call site with a compile-time- known fds array
// size into a call to the internal __poll_chk symbol instead of plain `poll`, silently bypassing the override above in
// optimized builds (confirmed: this is exactly what was happening on the Ubuntu x86_64 Release+gcc CI leg - our poll()
// hook was never being reached at all there). Intercept it too and funnel it through the same mock. `fds` here is a
// plain pointer parameter, not a locally-sized array, so glibc's fortify wrapper can't determine an object size for it
// and the call below falls through to the plain, unchecked path rather than recursing back into __poll_chk.
// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" __attribute__((no_sanitize("address"))) int __poll_chk(struct pollfd* fds, nfds_t nfds, int timeout,
                                                                  size_t /*fdslen*/) {
  return poll(fds, nfds, timeout);
}
#endif  // __GLIBC__

int getaddrinfo(const char* _name, const char* _service, const addrinfo* _req, addrinfo** _pai) {
  using GetAddrInfoFn = int (*)(const char*, const char*, const addrinfo*, addrinfo**);
  static GetAddrInfoFn real_getaddrinfo = reinterpret_cast<GetAddrInfoFn>(dlsym(RTLD_NEXT, "getaddrinfo"));
  if (real_getaddrinfo == nullptr) {
    std::abort();
  }
  if (!gHookState.addrinfoOverride.has_value()) {
    return real_getaddrinfo(_name, _service, _req, _pai);
  }
  const AddrinfoOverrideData& localOverride = *gHookState.addrinfoOverride;
  if (localOverride.result != 0) {
    *_pai = nullptr;
    return localOverride.result;
  }
  TestAddrinfoNode* head = DuplicateEntries(localOverride.entries);
  addrinfo* headAddr = head != nullptr ? &head->ai : nullptr;
  if (headAddr != nullptr) {
    gHookState.customHeads.insert(headAddr);
  }
  *_pai = headAddr;
  return 0;
}

void freeaddrinfo(addrinfo* _ai) {
  using FreeAddrInfoFn = void (*)(addrinfo*);
  static FreeAddrInfoFn real_freeaddrinfo = reinterpret_cast<FreeAddrInfoFn>(dlsym(RTLD_NEXT, "freeaddrinfo"));
  if (_ai == nullptr) {
    return;
  }
  auto it = gHookState.customHeads.find(_ai);
  if (it != gHookState.customHeads.end()) {
    gHookState.customHeads.erase(it);
    FreeCustomList(_ai);
    return;
  }
  if (real_freeaddrinfo == nullptr) {
    std::abort();
  }
  real_freeaddrinfo(_ai);
}

}  // extern "C"

namespace aeronet {
namespace {

using ::AddrinfoOverrideGuard;
using ::ConnectErr;
using ::HookGuard;
using ::MakeHostPortBuffer;
using ::MakeLoopback6Entry;
using ::MakeLoopbackEntry;
using ::PollErr;
using ::PollTimeout;
using ::ReserveClosedLoopbackPort;
using ::SetConnectActionSequence;
using ::SetPollActionSequence;
using ::SetSocketErrorSequence;

TEST(TcpConnectorTest, ResolutionFailureMarksFailure) {
  HookGuard guard;
  auto override = AddrinfoOverrideGuard::WithError(EAI_FAIL);
  auto buffer = MakeHostPortBuffer("invalid-host", 8080);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  EXPECT_FALSE(result.cnx);
}

TEST(TcpConnectorTest, SocketEmfileStopsIteration) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(9)});
  SetSocketErrorSequence({error::kTooManyFiles});
  auto buffer = MakeHostPortBuffer("loopback", 9);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  EXPECT_FALSE(result.cnx);
}

TEST(TcpConnectorTest, SocketEnfileStopsIteration) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(9)});
  SetSocketErrorSequence({ENFILE});
  auto buffer = MakeHostPortBuffer("loopback", 9);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  EXPECT_FALSE(result.cnx);
}

TEST(TcpConnectorTest, SocketErrorContinuesToNextAddress) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(10000), MakeLoopbackEntry(10001)});
  SetSocketErrorSequence({EACCES});
  SetConnectActionSequence({ConnectErr(ECONNREFUSED)});
  auto buffer = MakeHostPortBuffer("loopback", 10000);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  if (result.cnx) {
    result.cnx.close();
  }
}

TEST(TcpConnectorTest, ConnectSucceedsImmediately) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(15000)});
  SetConnectActionSequence({ConnectSuccess()});
  auto buffer = MakeHostPortBuffer("127.0.0.1", 15000);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_INET);
  EXPECT_FALSE(result.failure);
  EXPECT_FALSE(result.connectPending);
  ASSERT_TRUE(result.cnx);
  result.cnx.close();
}

TEST(TcpConnectorTest, ConnectReportsPendingWhenInProgress) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(11000)});
  SetConnectActionSequence({ConnectErr(error::kInProgress)});
  auto buffer = MakeHostPortBuffer("loopback", 11000);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_FALSE(result.failure);
  EXPECT_TRUE(result.connectPending);
  ASSERT_TRUE(result.cnx);
  result.cnx.close();
}

TEST(TcpConnectorTest, ConnectReportsPendingWhenAlready) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(11111)});
  SetConnectActionSequence({ConnectErr(EALREADY)});
  auto buffer = MakeHostPortBuffer("loopback", 11111);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_FALSE(result.failure);
  EXPECT_TRUE(result.connectPending);
  ASSERT_TRUE(result.cnx);
  result.cnx.close();
}

TEST(TcpConnectorTest, ConnectRetriesAfterEintrAndSucceeds) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(16000), MakeLoopbackEntry(16001)});
  SetConnectActionSequence({ConnectErr(error::kInterrupted), ConnectSuccess()});
  auto buffer = MakeHostPortBuffer("127.0.0.1", 16000);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_FALSE(result.failure);
  EXPECT_FALSE(result.connectPending);
  ASSERT_TRUE(result.cnx);
  result.cnx.close();
}

TEST(TcpConnectorTest, ConnectFailureSetsFailureFlag) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(12000)});
  SetConnectActionSequence({ConnectErr(ECONNREFUSED)});
  auto buffer = MakeHostPortBuffer("loopback", 12000);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  if (result.cnx) {
    result.cnx.close();
  }
}

// The regression at the heart of the dual-stack CI failure: getaddrinfo yields ::1 before 127.0.0.1, but
// the server only listens on IPv4. With a connect-timeout budget the connector must drive the refused ::1
// connect to completion and fall back to 127.0.0.1 — returning an established socket, not connectPending.
TEST(TcpConnectorTest, BlockingFallbackTriesNextAddressOnRefusal) {
  HookGuard guard;
  // Real IPv4 loopback listener; nothing listens on ::1.
  const BaseFd listener(::socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(listener);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(::bind(listener.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&addr), &len), 0);
  ASSERT_EQ(::listen(listener.fd(), 16), 0);
  const uint16_t port = ntohs(addr.sin_port);

  // Real connect()/poll() (no action sequence armed): ::1 is refused, 127.0.0.1 reaches the listener.
  AddrinfoOverrideGuard override({MakeLoopback6Entry(port), MakeLoopbackEntry(port)});
  auto buffer = MakeHostPortBuffer("localhost", 0);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC, /*connectTimeoutMs=*/2000);
  EXPECT_FALSE(result.failure);
  EXPECT_FALSE(result.connectPending);
  ASSERT_TRUE(result.cnx);
  result.cnx.close();
}

// When every resolved candidate is refused, the blocking fallback exhausts the list and reports failure
// (rather than handing back a half-open pending socket).
TEST(TcpConnectorTest, BlockingFallbackFailsWhenAllCandidatesRefused) {
  HookGuard guard;
  const uint16_t deadPort = ReserveClosedLoopbackPort();
  AddrinfoOverrideGuard override({MakeLoopback6Entry(deadPort), MakeLoopbackEntry(deadPort)});
  auto buffer = MakeHostPortBuffer("localhost", 0);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC, /*connectTimeoutMs=*/1000);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  EXPECT_FALSE(result.cnx);
}

// Covers the fallthrough at connectErr == error::kWouldBlock (only reachable in practice on
// Windows, where a non-blocking connect() reports WSAEWOULDBLOCK instead of EINPROGRESS).
TEST(TcpConnectorTest, ConnectWouldBlockTreatedAsInProgress) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(14000)});
  SetConnectActionSequence({ConnectErr(error::kWouldBlock)});
  auto buffer = MakeHostPortBuffer("loopback", 14000);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC);  // blockingFallback == false
  EXPECT_FALSE(result.failure);
  EXPECT_TRUE(result.connectPending);
  ASSERT_TRUE(result.cnx);
  result.cnx.close();
}

// poll() itself reporting "no events" (its own per-call budget elapsed) is a distinct code path
// from the overall connect deadline elapsing; covers WaitForConnectCompletion's pr == 0 branch.
TEST(TcpConnectorTest, BlockingFallbackTimesOutWhenPollReportsNoEvents) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(13003)});
  SetConnectActionSequence({ConnectErr(error::kInProgress)});
  SetPollActionSequence({PollTimeout()});
  auto buffer = MakeHostPortBuffer("loopback", 13003);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC, /*connectTimeoutMs=*/2000);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  EXPECT_FALSE(result.cnx);
}

// A fatal poll() error (not EINTR) marks just that candidate as failed and moves on; with both
// candidates failing this way the overall result is failure. Covers pr < 0 with errno != EINTR.
TEST(TcpConnectorTest, BlockingFallbackTreatsPollErrorAsCandidateFailure) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(13001), MakeLoopbackEntry(13002)});
  SetConnectActionSequence({ConnectErr(error::kInProgress), ConnectErr(error::kInProgress)});
  SetPollActionSequence({PollErr(EFAULT), PollErr(EFAULT)});
  auto buffer = MakeHostPortBuffer("loopback", 13001);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC, /*connectTimeoutMs=*/2000);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  EXPECT_FALSE(result.cnx);
}

// poll() reports EINTR but is made to "block" long enough that by the time the loop re-checks
// the (tiny) overall deadline, the budget has already elapsed. Covers the deadline check firing
// mid-loop rather than only on entry, and locks in that EINTR retries instead of failing outright.
TEST(TcpConnectorTest, BlockingFallbackTimesOutWhenDeadlineElapsesBetweenPollCalls) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(13000)});
  SetConnectActionSequence({ConnectErr(error::kInProgress)});
  SetPollActionSequence({PollErr(error::kInterrupted, std::chrono::milliseconds(30))});
  auto buffer = MakeHostPortBuffer("loopback", 13000);
  ConnectResult result = ConnectTCP(buffer.host, buffer.port, AF_UNSPEC, /*connectTimeoutMs=*/1);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  EXPECT_FALSE(result.cnx);
}

}  // namespace
}  // namespace aeronet
