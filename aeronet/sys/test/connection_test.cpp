#include "aeronet/connection.hpp"

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <optional>

#include "aeronet/socket.hpp"
#include "aeronet/sys_test_support.hpp"

using namespace aeronet;
namespace test_support = aeronet::sys::test_support;

namespace {

struct AcceptAction {
  enum class Kind : std::uint8_t { Fd, Error };
  Kind kind{Kind::Fd};
  int value{0};
};

[[nodiscard]] AcceptAction AcceptFd(int fd) { return AcceptAction{AcceptAction::Kind::Fd, fd}; }
[[nodiscard]] AcceptAction AcceptErr(int err) { return AcceptAction{AcceptAction::Kind::Error, err}; }

test_support::ActionQueue<AcceptAction> gAcceptActions;

void SetAcceptActionSequence(std::initializer_list<AcceptAction> actions) { gAcceptActions.setActions(actions); }

class AcceptHookGuard : public test_support::QueueResetGuard<test_support::ActionQueue<AcceptAction>> {
 public:
  AcceptHookGuard() : QueueResetGuard(gAcceptActions) {}
};

}  // namespace

// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-inconsistent-declaration-parameter-name)
extern "C" int accept4(int __fd, struct sockaddr* __addr, socklen_t* __addr_len, int __flags) {
  using AcceptFn = int (*)(int, struct sockaddr*, socklen_t*, int);
  static AcceptFn real_accept4 = reinterpret_cast<AcceptFn>(dlsym(RTLD_NEXT, "accept4"));
  if (real_accept4 == nullptr) {
    std::abort();
  }

  const auto action = gAcceptActions.pop();
  if (action.has_value()) {
    if (action->kind == AcceptAction::Kind::Fd) {
      return action->value;
    }
    errno = action->value;
    return -1;
  }
  return real_accept4(__fd, __addr, __addr_len, __flags);
}

TEST(ConnectionTest, AcceptWouldBlockYieldsEmptyConnection) {
  AcceptHookGuard guard;
  SetAcceptActionSequence({AcceptErr(EAGAIN)});
  Socket listener(SOCK_STREAM);

  Connection conn(listener);
  EXPECT_FALSE(conn);
}

TEST(ConnectionTest, AcceptFatalErrorYieldsFailure) {
  AcceptHookGuard guard;
  SetAcceptActionSequence({AcceptErr(EPERM)});
  Socket listener(SOCK_STREAM);

  Connection conn(listener);
  EXPECT_FALSE(conn);
}

TEST(ConnectionTest, AcceptSuccessAdoptsFd) {
  AcceptHookGuard guard;
  const int fakeFd = test_support::CreateMemfd("aeronet-accept");
  ASSERT_GE(fakeFd, 0);
  SetAcceptActionSequence({AcceptFd(fakeFd)});

  Socket listener(SOCK_STREAM);
  Connection conn(listener);
  ASSERT_TRUE(conn);
  EXPECT_EQ(conn.fd(), fakeFd);
  conn.close();
}
