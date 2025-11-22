#include "aeronet/tcp-connector.hpp"

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct ConnectAction {
  enum class Kind : std::uint8_t { Real, Error, Success };
  Kind kind{Kind::Real};
  int err{0};
};

[[nodiscard]] ConnectAction ConnectErr(int err) { return ConnectAction{ConnectAction::Kind::Error, err}; }
[[nodiscard]] ConnectAction ConnectSuccess() { return ConnectAction{ConnectAction::Kind::Success, 0}; }

struct TestAddrEntry {
  sockaddr_storage storage{};
  socklen_t addrlen{0};
  int family{AF_INET};
  int sockType{SOCK_STREAM};
  int protocol{0};
};

[[nodiscard]] TestAddrEntry MakeLoopbackEntry(uint16_t port) {
  TestAddrEntry entry;
  auto *sin = reinterpret_cast<sockaddr_in *>(&entry.storage);
  sin->sin_family = AF_INET;
  sin->sin_port = htons(port);
  sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  entry.addrlen = sizeof(sockaddr_in);
  entry.family = AF_INET;
  entry.sockType = SOCK_STREAM;
  entry.protocol = IPPROTO_TCP;
  return entry;
}

struct AddrinfoOverrideData {
  int result{0};
  std::vector<TestAddrEntry> entries;
};

struct TestAddrinfoNode {
  addrinfo ai{};
  sockaddr_storage storage{};
};

struct HookState {
  std::deque<int> socketErrnos;
  std::deque<ConnectAction> connectActions;
  std::optional<AddrinfoOverrideData> addrinfoOverride;
  std::unordered_set<addrinfo *> customHeads;
};

HookState gHookState;
std::mutex gHookMutex;

[[nodiscard]] TestAddrinfoNode *DuplicateEntries(const std::vector<TestAddrEntry> &entries) {
  TestAddrinfoNode *head = nullptr;
  TestAddrinfoNode *tail = nullptr;
  for (const auto &entry : entries) {
    auto *node = new TestAddrinfoNode();
    node->ai.ai_family = entry.family;
    node->ai.ai_socktype = entry.sockType;
    node->ai.ai_protocol = entry.protocol;
    node->ai.ai_addrlen = entry.addrlen;
    node->storage = entry.storage;
    node->ai.ai_addr = reinterpret_cast<sockaddr *>(&node->storage);
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

void FreeCustomList(addrinfo *head) {
  auto *node = reinterpret_cast<TestAddrinfoNode *>(head);
  while (node != nullptr) {
    auto *next = node->ai.ai_next != nullptr ? reinterpret_cast<TestAddrinfoNode *>(node->ai.ai_next) : nullptr;
    delete node;
    node = next;
  }
}

void ResetHooks() {
  std::scoped_lock lock(gHookMutex);
  gHookState.socketErrnos.clear();
  gHookState.connectActions.clear();
  gHookState.addrinfoOverride.reset();
  for (addrinfo *head : gHookState.customHeads) {
    FreeCustomList(head);
  }
  gHookState.customHeads.clear();
}

class HookGuard {
 public:
  HookGuard() { ResetHooks(); }
  HookGuard(const HookGuard &) = delete;
  HookGuard &operator=(const HookGuard &) = delete;
  ~HookGuard() { ResetHooks(); }
};

void SetSocketErrorSequence(std::initializer_list<int> errs) {
  std::scoped_lock lock(gHookMutex);
  gHookState.socketErrnos.assign(errs.begin(), errs.end());
}

void SetConnectActionSequence(std::initializer_list<ConnectAction> actions) {
  std::scoped_lock lock(gHookMutex);
  gHookState.connectActions.assign(actions.begin(), actions.end());
}

class AddrinfoOverrideGuard {
 public:
  AddrinfoOverrideGuard() = default;

  explicit AddrinfoOverrideGuard(std::vector<TestAddrEntry> entries, int result = 0) {
    activate(std::move(entries), result);
  }

  AddrinfoOverrideGuard(const AddrinfoOverrideGuard &) = delete;
  AddrinfoOverrideGuard &operator=(const AddrinfoOverrideGuard &) = delete;

  AddrinfoOverrideGuard(AddrinfoOverrideGuard &&other) noexcept { active_ = std::exchange(other.active_, false); }

  AddrinfoOverrideGuard &operator=(AddrinfoOverrideGuard &&other) noexcept {
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
    std::scoped_lock lock(gHookMutex);
    gHookState.addrinfoOverride.reset();
    active_ = false;
  }

 private:
  bool active_{false};

  void activate(std::vector<TestAddrEntry> entries, int result) {
    std::scoped_lock lock(gHookMutex);
    gHookState.addrinfoOverride = AddrinfoOverrideData{result, std::move(entries)};
    active_ = true;
  }
};

struct HostPortBuffer {
  std::string storage;
  std::string_view host;
  std::string_view port;
};

[[nodiscard]] HostPortBuffer MakeHostPortBuffer(const std::string &host, const std::string &port) {
  HostPortBuffer buffer;
  buffer.storage.reserve(host.size() + port.size() + 2);
  buffer.storage.append(host);
  buffer.storage.push_back('\0');
  const std::size_t portOffset = buffer.storage.size();
  buffer.storage.append(port);
  buffer.storage.push_back('\0');
  buffer.host = std::string_view(buffer.storage.data(), host.size());
  buffer.port = std::string_view(buffer.storage.data() + portOffset, port.size());
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
  {
    std::scoped_lock lock(gHookMutex);
    if (!gHookState.socketErrnos.empty()) {
      err = gHookState.socketErrnos.front();
      gHookState.socketErrnos.pop_front();
    }
  }
  if (err != 0) {
    errno = err;
    return -1;
  }
  return real_socket(_domain, _type, _protocol);
}

int connect(int _fd, const sockaddr *_addr, socklen_t _len) {
  using ConnectFn = int (*)(int, const sockaddr *, socklen_t);
  static ConnectFn real_connect = reinterpret_cast<ConnectFn>(dlsym(RTLD_NEXT, "connect"));
  if (real_connect == nullptr) {
    std::abort();
  }
  ConnectAction action;
  bool hasAction = false;
  {
    std::scoped_lock lock(gHookMutex);
    if (!gHookState.connectActions.empty()) {
      action = gHookState.connectActions.front();
      gHookState.connectActions.pop_front();
      hasAction = true;
    }
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

int getaddrinfo(const char *_name, const char *_service, const addrinfo *_req, addrinfo **_pai) {
  using GetAddrInfoFn = int (*)(const char *, const char *, const addrinfo *, addrinfo **);
  static GetAddrInfoFn real_getaddrinfo = reinterpret_cast<GetAddrInfoFn>(dlsym(RTLD_NEXT, "getaddrinfo"));
  if (real_getaddrinfo == nullptr) {
    std::abort();
  }
  std::optional<AddrinfoOverrideData> localOverride;
  {
    std::scoped_lock lock(gHookMutex);
    if (gHookState.addrinfoOverride.has_value()) {
      localOverride = gHookState.addrinfoOverride;
    }
  }
  if (!localOverride) {
    return real_getaddrinfo(_name, _service, _req, _pai);
  }
  if (localOverride->result != 0) {
    *_pai = nullptr;
    return localOverride->result;
  }
  TestAddrinfoNode *head = DuplicateEntries(localOverride->entries);
  addrinfo *headAddr = head != nullptr ? &head->ai : nullptr;
  if (headAddr != nullptr) {
    std::scoped_lock lock(gHookMutex);
    gHookState.customHeads.insert(headAddr);
  }
  *_pai = headAddr;
  return 0;
}

void freeaddrinfo(addrinfo *_ai) {
  using FreeAddrInfoFn = void (*)(addrinfo *);
  static FreeAddrInfoFn real_freeaddrinfo = reinterpret_cast<FreeAddrInfoFn>(dlsym(RTLD_NEXT, "freeaddrinfo"));
  if (_ai == nullptr) {
    return;
  }
  {
    std::scoped_lock lock(gHookMutex);
    auto it = gHookState.customHeads.find(_ai);
    if (it != gHookState.customHeads.end()) {
      gHookState.customHeads.erase(it);
      FreeCustomList(_ai);
      return;
    }
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
using ::MakeLoopbackEntry;
using ::SetConnectActionSequence;
using ::SetSocketErrorSequence;

TEST(TcpConnectorTest, ResolutionFailureMarksFailure) {
  HookGuard guard;
  auto override = AddrinfoOverrideGuard::WithError(EAI_FAIL);
  auto buffer = MakeHostPortBuffer("invalid-host", "8080");
  ConnectResult result = ConnectTCP(buffer.storage.data(), buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  EXPECT_FALSE(result.cnx);
}

TEST(TcpConnectorTest, SocketEmfileStopsIteration) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(9)});
  SetSocketErrorSequence({EMFILE});
  auto buffer = MakeHostPortBuffer("loopback", "9");
  ConnectResult result = ConnectTCP(buffer.storage.data(), buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  EXPECT_FALSE(result.cnx);
}

TEST(TcpConnectorTest, SocketErrorContinuesToNextAddress) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(10000), MakeLoopbackEntry(10001)});
  SetSocketErrorSequence({EACCES});
  SetConnectActionSequence({ConnectErr(ECONNREFUSED)});
  auto buffer = MakeHostPortBuffer("loopback", "10000");
  ConnectResult result = ConnectTCP(buffer.storage.data(), buffer.host, buffer.port, AF_UNSPEC);
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
  auto buffer = MakeHostPortBuffer("127.0.0.1", "15000");
  ConnectResult result = ConnectTCP(buffer.storage.data(), buffer.host, buffer.port, AF_INET);
  EXPECT_FALSE(result.failure);
  EXPECT_FALSE(result.connectPending);
  ASSERT_TRUE(result.cnx);
  result.cnx.close();
}

TEST(TcpConnectorTest, ConnectReportsPendingWhenInProgress) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(11000)});
  SetConnectActionSequence({ConnectErr(EINPROGRESS)});
  auto buffer = MakeHostPortBuffer("loopback", "11000");
  ConnectResult result = ConnectTCP(buffer.storage.data(), buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_FALSE(result.failure);
  EXPECT_TRUE(result.connectPending);
  ASSERT_TRUE(result.cnx);
  result.cnx.close();
}

TEST(TcpConnectorTest, ConnectRetriesAfterEintrAndSucceeds) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(16000), MakeLoopbackEntry(16001)});
  SetConnectActionSequence({ConnectErr(EINTR), ConnectSuccess()});
  auto buffer = MakeHostPortBuffer("127.0.0.1", "16000");
  ConnectResult result = ConnectTCP(buffer.storage.data(), buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_FALSE(result.failure);
  EXPECT_FALSE(result.connectPending);
  ASSERT_TRUE(result.cnx);
  result.cnx.close();
}

TEST(TcpConnectorTest, ConnectFailureSetsFailureFlag) {
  HookGuard guard;
  AddrinfoOverrideGuard override({MakeLoopbackEntry(12000)});
  SetConnectActionSequence({ConnectErr(ECONNREFUSED)});
  auto buffer = MakeHostPortBuffer("loopback", "12000");
  ConnectResult result = ConnectTCP(buffer.storage.data(), buffer.host, buffer.port, AF_UNSPEC);
  EXPECT_TRUE(result.failure);
  EXPECT_FALSE(result.connectPending);
  if (result.cnx) {
    result.cnx.close();
  }
}

}  // namespace
}  // namespace aeronet
