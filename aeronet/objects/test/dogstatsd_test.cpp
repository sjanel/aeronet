#include "aeronet/dogstatsd.hpp"

#include <gtest/gtest.h>
#include "aeronet/platform.hpp"

#ifdef AERONET_POSIX
#include <sys/un.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#define AERONET_WANT_SOCKET_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#include "aeronet/unix-dogstatsd-sink.hpp"

namespace aeronet {

TEST(DogStatsDTest, DefaultConstructorDisabled) {
  DogStatsD client;
  EXPECT_NO_THROW(client.increment("noop"));
  EXPECT_NO_THROW(client.gauge("noop", 1.0));
  EXPECT_NO_THROW(client.histogram("noop", 2.0));
  EXPECT_NO_THROW(client.timing("noop", std::chrono::milliseconds{1}));
  EXPECT_NO_THROW(client.set("noop", "value"));
}

TEST(DogStatsDTest, EmptyNamespace) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "");

  client.increment("requests");
  EXPECT_EQ(sink.recvMessage(), "requests:1|c");
}

TEST(DogStatsDTest, SocketFails) {
  test::PushSocketAction({-1, EMFILE});  // EMFILE: too many open files
  EXPECT_THROW(DogStatsD("/invalid/path/to/socket.sock", "svc"), std::system_error);
}

TEST(DogStatsDTest, SocketTimeout) {
  // Simulate missing socket file: constructor should not throw, it should
  // defer retry (ENOENT is a connectivity/runtime condition).
  test::PushConnectAction({-1, ENOENT});
  EXPECT_NO_THROW(DogStatsD("/invalid/path/to/socket.sock", "svc"));
}

TEST(DogStatsDTest, SocketInvalidFormatThrows) {
  // Simulate a structural/path format error (ENOTDIR) that should be
  // considered a configuration error and cause the constructor to throw.
  test::PushConnectAction({-1, ENOTDIR});
  EXPECT_THROW(DogStatsD("/invalid/path/to/socket.sock", "svc"), std::invalid_argument);
}

TEST(DogStatsDTest, SocketInvalid_EISDIR) {
  test::PushConnectAction({-1, EISDIR});
  EXPECT_THROW(DogStatsD("/invalid/path/to/socket.sock", "svc"), std::invalid_argument);
}

TEST(DogStatsDTest, SocketInvalid_ELOOP) {
  test::PushConnectAction({-1, ELOOP});
  EXPECT_THROW(DogStatsD("/invalid/path/to/socket.sock", "svc"), std::invalid_argument);
}

TEST(DogStatsDTest, SocketInvalid_EINVAL) {
  test::PushConnectAction({-1, EINVAL});
  EXPECT_THROW(DogStatsD("/invalid/path/to/socket.sock", "svc"), std::invalid_argument);
}

TEST(DogStatsDTest, SocketInvalid_ENOTSOCK) {
  test::PushConnectAction({-1, ENOTSOCK});
  EXPECT_THROW(DogStatsD("/invalid/path/to/socket.sock", "svc"), std::invalid_argument);
}

TEST(DogStatsDTest, SocketInvalid_EACCES) {
  test::PushConnectAction({-1, EACCES});
  EXPECT_THROW(DogStatsD("/invalid/path/to/socket.sock", "svc"), std::invalid_argument);
}

TEST(DogStatsDTest, SocketInvalid_EPERM) {
  test::PushConnectAction({-1, EPERM});
  EXPECT_THROW(DogStatsD("/invalid/path/to/socket.sock", "svc"), std::invalid_argument);
}

TEST(DogStatsDTest, SendMetricFailsToAllocateMemory) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");

  test::FailNextRealloc(1);  // cause next realloc to fail

  // Should not throw despite allocation failure
  EXPECT_NO_THROW(
      client.increment("a-very-long-metric-name-to-trigger-allocation-because-it-will-allocate-already-a-buffer-of-"
                       "some-dozens-of-additional-chars-at-constructor-time"));
}

TEST(DogStatsDTest, SendSystemError) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");

  // Inject send failure (EBADF - bad file descriptor)
  test::PushSendAction({-1, EBADF});

  // Should not throw despite send() failure
  EXPECT_NO_THROW(client.increment("requests"));
}

TEST(DogStatsDTest, SendsAllMetricTypesWithTags) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  DogStatsD::DogStatsDTags tags;
  tags.append("env:dev");
  tags.append("role:web");

  client.increment("hits", 3, tags);
  EXPECT_EQ(sink.recvMessage(), "svc.hits:3|c|#env:dev,role:web");

  client.gauge("temp", 12, tags);
  EXPECT_EQ(sink.recvMessage(), "svc.temp:12|g|#env:dev,role:web");

  client.histogram("payload", 4.25, {});
  EXPECT_EQ(sink.recvMessage(), "svc.payload:4.25|h");

  client.timing("latency", std::chrono::milliseconds{42}, {});
  EXPECT_EQ(sink.recvMessage(), "svc.latency:42|ms");

  client.set("users", "abc", tags);
  EXPECT_EQ(sink.recvMessage(), "svc.users:abc|s|#env:dev,role:web");
}

TEST(DogStatsDTest, RespectsExistingNamespaceDotAndEmptyTags) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc.");

  client.increment("requests");
  EXPECT_EQ(sink.recvMessage(), "svc.requests:1|c");
}

TEST(DogStatsDTest, EmptySocketPathDisablesClient) {
  DogStatsD client({}, "svc");
  EXPECT_NO_THROW(client.increment("noop"));
  EXPECT_NO_THROW(client.gauge("noop", 1.0));
  EXPECT_NO_THROW(client.histogram("noop", 2.0));
  EXPECT_NO_THROW(client.timing("noop", std::chrono::milliseconds{1}));
  EXPECT_NO_THROW(client.set("noop", "value"));
}

TEST(DogStatsDTest, RejectsTooLongSocketPath) {
  std::string veryLong(sizeof(sockaddr_un{}.sun_path), 'a');
  EXPECT_THROW(DogStatsD(veryLong, {}), std::invalid_argument);
}

TEST(DogStatsDTest, RejectsTooLongNamespace) {
  test::UnixDogstatsdSink sink;
  std::string ns(256, 'n');
  EXPECT_THROW(DogStatsD(sink.path(), ns), std::invalid_argument);
}

TEST(DogStatsDTest, SendFailureLogsAndContinues) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  sink.closeAndUnlink();
  EXPECT_NO_THROW(client.increment("lost", 1));
}

TEST(DogStatsDTest, SendEagainIsDropped) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");

  // Inject EAGAIN for the next send; the client should treat it as a dropped metric
  // and not mark the connection for immediate reconnect. No exception should be thrown.
  test::PushSendAction({-1, EAGAIN});
  EXPECT_NO_THROW(client.increment("lost", 1));

  // Subsequent sends should still work (no socket teardown on EAGAIN)
  client.increment("ok", 1);
  EXPECT_EQ(sink.recvMessage(), "svc.ok:1|c");
}

TEST(DogStatsDTest, IfFirstConnectFailsReconnectShouldBeAttemptedOnNextSend) {
  // Create a sink but arrange for the first connect() to fail. The DogStatsD
  // constructor calls tryReconnect(), which will pop the first connect action.
  // We then send many messages; after enough sends the client will attempt
  // reconnects periodically (controlled by kReconnectionThreshold). We push
  // a successful connect action after some failures and expect at least one
  // message to be received by the sink.

  test::UnixDogstatsdSink sink;

  // First connect attempt (from constructor) fails with ENOENT.
  test::PushConnectAction({-1, ENOENT});

  // Create client: initial connect will fail but object should be constructed.
  DogStatsD client(sink.path(), "svc");

  // Now arrange for subsequent connect attempts to succeed once we want them to.
  // We'll not push a success yet — the real connect will attempt to connect to
  // the sink's path (which is valid) when the action queue is empty. To ensure
  // reconnect eventually succeeds, push a success action after some messages.

  // Send many messages to trigger reconnect attempts. We don't want this test to
  // take too long, so pick a number greater than kReconnectionThreshold (80).
  client.increment("requests");

  // At least one of the sends after connect success should be received by the sink.
  // Allow small timeout for delivery.
  const std::string msg = sink.recvMessage(500);
  EXPECT_TRUE(!msg.empty());
}

TEST(DogStatsDTest, RetryShouldBePeriodic) {
  test::UnixDogstatsdSink sink;

  // First connect attempt (from constructor) fails with ENOENT.
  test::PushConnectAction({-1, ENOENT});

  // Create client: initial connect will fail but object should be constructed.
  DogStatsD client(sink.path(), "svc");

  // Also make immediate next connect attempt fail.
  test::PushConnectAction({-1, ENOENT});

  // Now arrange for subsequent connect attempts to succeed once we want them to.
  // We'll not push a success yet — the real connect will attempt to connect to
  // the sink's path (which is valid) when the action queue is empty. To ensure
  // reconnect eventually succeeds, push a success action after some messages.
  for (int messagePos = 0; messagePos < 49; ++messagePos) {
    client.increment("requests");
  }

  // None of the sends should be received by the sink yet.
  std::string msg = sink.recvMessage(100);
  EXPECT_TRUE(msg.empty());

  // next one should trigger a reconnect attempt that will succeed.
  client.increment("requests");

  // At least one of the sends after connect success should be received by the sink.
  // Allow small timeout for delivery.
  msg = sink.recvMessage(500);
  EXPECT_FALSE(msg.empty());
}

}  // namespace aeronet
