#include "base-fd.hpp"

#include <gtest/gtest.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace aeronet;

namespace {
int CreateMemfd(std::string_view name) {
  const std::string nameStr(name);
  int retfd = static_cast<int>(::syscall(SYS_memfd_create, nameStr.c_str(), MFD_CLOEXEC));
  if (retfd < 0) {
    throw std::runtime_error("memfd_create failed: " + std::string(std::strerror(errno)));
  }
  return retfd;
}
}  // namespace

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
  const int fd = CreateMemfd("aeronet-memfd-bool");
  ASSERT_GE(fd, 0);

  BaseFd fdOwner3(fd);
  EXPECT_TRUE(fdOwner3);
  const int raw = fdOwner3.release();
  EXPECT_FALSE(fdOwner3);
  EXPECT_GE(raw, 0);
  ::close(raw);
}
