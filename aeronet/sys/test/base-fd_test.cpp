#include "aeronet/base-fd.hpp"

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "aeronet/sys-test-support.hpp"

using namespace aeronet;

namespace {
struct CloseAction {
  enum class Kind : std::uint8_t { Error };
  Kind kind{Kind::Error};
  int err{0};
};

[[nodiscard]] CloseAction CloseErr(int err) { return CloseAction{CloseAction::Kind::Error, err}; }

test::KeyedActionQueue<int, CloseAction> gCloseOverrides;

void SetCloseErrorSequence(int fd, std::initializer_list<CloseAction> actions) {
  gCloseOverrides.setActions(fd, actions);
}

class CloseOverrideGuard : public test::QueueResetGuard<test::KeyedActionQueue<int, CloseAction>> {
 public:
  CloseOverrideGuard() : QueueResetGuard(gCloseOverrides) {}
};

using CloseFn = int (*)(int);

CloseFn GetRealClose() {
  static CloseFn real_close = reinterpret_cast<CloseFn>(dlsym(RTLD_NEXT, "close"));
  if (real_close == nullptr) {
    std::abort();
  }
  return real_close;
}

int CallRealClose(int fd) { return GetRealClose()(fd); }

}  // namespace

extern "C" int close(int fd) {
  const auto action = gCloseOverrides.pop(fd);
  if (action.has_value() && action->kind == CloseAction::Kind::Error) {
    errno = action->err;
    return -1;
  }
  return GetRealClose()(fd);
}

TEST(BaseFd, ReleaseMakesObjectClosedAndReturnsFd) {
  int fds[2];
  ASSERT_EQ(0, ::pipe(fds));
  // wrap the read end
  BaseFd rd(fds[0]);
  // The pipe returns two fds; ownership was transferred to BaseFd, so we must
  // close the write end manually to avoid leak.
  ::close(fds[1]);

  ASSERT_TRUE(rd);
  int raw = rd.release();
  // After release, object must be closed
  EXPECT_FALSE(rd);
  // raw must be a valid fd
  EXPECT_GE(raw, 0);
  // closing raw should succeed
  EXPECT_EQ(0, ::close(raw));
}

TEST(BaseFd, ReleaseOnClosedReturnsClosedSentinel) {
  BaseFd empty;  // default closed
  EXPECT_FALSE(empty);
  const int rv = empty.release();
  EXPECT_EQ(rv, BaseFd::kClosedFd);
}

TEST(BaseFd, BoolOperatorAndReleaseIntegration) {
  const int fd = test::CreateMemfd("aeronet-memfd-bool");
  ASSERT_GE(fd, 0);

  BaseFd fdOwner3(fd);
  EXPECT_TRUE(fdOwner3);
  const int raw = fdOwner3.release();
  EXPECT_FALSE(fdOwner3);
  EXPECT_GE(raw, 0);
  ::close(raw);
}

TEST(BaseFd, DestroyShouldLogIfFdAlreadyClosed) {
  const int fd = test::CreateMemfd("aeronet-memfd-bool");
  ASSERT_GE(fd, 0);

  BaseFd fdOwner(fd);

  ::close(fd);  // close before destruction to simulate double-close
}

TEST(BaseFd, MoveAssignSelfNoOpLeavesFdIntact) {
  const int fd = test::CreateMemfd("aeronet-memfd-self-move");
  ASSERT_GE(fd, 0);

  BaseFd fdOwner(fd);
  auto& alias = fdOwner;
  fdOwner = std::move(alias);
  EXPECT_TRUE(fdOwner);
  fdOwner.close();
}

TEST(BaseFd, CloseRetriesAfterEintr) {
  CloseOverrideGuard guard;
  const int fd = test::CreateMemfd("aeronet-memfd-eintr");
  ASSERT_GE(fd, 0);

  SetCloseErrorSequence(fd, {CloseErr(EINTR)});

  BaseFd fdOwner(fd);
  fdOwner.close();
  EXPECT_FALSE(fdOwner);
}

TEST(BaseFd, CloseLogsOtherErrorsButMarksClosed) {
  CloseOverrideGuard guard;
  const int fd = test::CreateMemfd("aeronet-memfd-error");
  ASSERT_GE(fd, 0);

  SetCloseErrorSequence(fd, {CloseErr(EBADF)});

  BaseFd fdOwner(fd);
  fdOwner.close();
  EXPECT_FALSE(fdOwner);

  ASSERT_EQ(0, CallRealClose(fd));
}