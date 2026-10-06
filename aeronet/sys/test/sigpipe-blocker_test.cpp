#include "aeronet/sigpipe-blocker.hpp"

#include <gtest/gtest.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers) POSIX API: sigset_t, pthread_sigmask, sigtimedwait
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <ctime>
#include <thread>

#include "aeronet/base-fd.hpp"
#include "aeronet/sigpipe-test-helpers.hpp"

namespace aeronet {

namespace {

// Writes to a socket whose peer is closed, which raises SIGPIPE in the calling thread.
void WriteToClosedPeer() {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  BaseFd writer(fds[1]);
  ::close(fds[0]);
  EXPECT_EQ(::write(fds[1], "x", 1), -1);
  EXPECT_EQ(errno, EPIPE);
}

sigset_t SigpipeSet() {  // NOLINT(misc-include-cleaner)
  sigset_t sigpipeSet;
  ::sigemptyset(&sigpipeSet);
  ::sigaddset(&sigpipeSet, SIGPIPE);
  return sigpipeSet;
}

}  // namespace

TEST(SigpipeBlocker, DiscardsSigpipeRaisedWhileBlocked) {
  test::DefaultSigpipeScope sigpipeScope;  // without the blocker, the write would terminate the test
  ASSERT_FALSE(test::IsSigpipeBlocked());
  {
    SigpipeBlocker blocker;
    EXPECT_TRUE(test::IsSigpipeBlocked());
    WriteToClosedPeer();
    EXPECT_TRUE(test::IsSigpipePending());
  }
  EXPECT_FALSE(test::IsSigpipeBlocked());
  EXPECT_FALSE(test::IsSigpipePending());
}

TEST(SigpipeBlocker, OnlyTheOutermostBlockerRestoresTheMask) {
  test::DefaultSigpipeScope sigpipeScope;
  {
    SigpipeBlocker outer;
    {
      SigpipeBlocker inner;
      WriteToClosedPeer();
    }
    EXPECT_TRUE(test::IsSigpipeBlocked());
    EXPECT_TRUE(test::IsSigpipePending());
  }
  EXPECT_FALSE(test::IsSigpipeBlocked());
  EXPECT_FALSE(test::IsSigpipePending());
}

TEST(SigpipeBlocker, DisabledBlockerDoesNothing) {
  {
    SigpipeBlocker disabled(false);
    EXPECT_FALSE(test::IsSigpipeBlocked());
    {
      SigpipeBlocker nested;  // the disabled blocker does not count as an outer one
      EXPECT_TRUE(test::IsSigpipeBlocked());
    }
    EXPECT_FALSE(test::IsSigpipeBlocked());
  }
  EXPECT_FALSE(test::IsSigpipeBlocked());
}

// A thread blocking SIGPIPE on its own keeps it blocked, and its pending SIGPIPE, for its own handling.
TEST(SigpipeBlocker, LeavesSigpipeToThreadsAlreadyBlockingIt) {
  const sigset_t sigpipeSet = SigpipeSet();
  ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &sigpipeSet, nullptr), 0);
  {
    SigpipeBlocker blocker;
    WriteToClosedPeer();
  }
  EXPECT_TRUE(test::IsSigpipeBlocked());
  EXPECT_TRUE(test::IsSigpipePending());

  static constexpr timespec kNoWait{};
  EXPECT_EQ(::sigtimedwait(&sigpipeSet, nullptr, &kNoWait), SIGPIPE);
  ASSERT_EQ(::pthread_sigmask(SIG_UNBLOCK, &sigpipeSet, nullptr), 0);
}

TEST(SigpipeBlocker, OnlyAffectsTheCallingThread) {
  std::atomic<bool> blockerCreated{false};
  std::atomic<bool> blockedInOtherThread{true};
  std::thread other([&] {
    while (!blockerCreated.load()) {
      std::this_thread::yield();
    }
    blockedInOtherThread.store(test::IsSigpipeBlocked());
  });
  {
    SigpipeBlocker blocker;
    blockerCreated.store(true);
    other.join();
  }
  EXPECT_FALSE(blockedInOtherThread.load());
}

}  // namespace aeronet
