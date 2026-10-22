#include "aeronet/dogstatsd.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#define AERONET_WANT_SOCKET_OVERRIDES

#include "aeronet/metric-label.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/unix-dogstatsd-sink.hpp"

#ifdef AERONET_POSIX
#include <sys/un.h>
#endif

namespace aeronet {

TEST(DogStatsDTest, DefaultConstructorDisabled) {
  DogStatsD client;
  const MetricLabel labels[]{{"protocol", "h2"}};
  EXPECT_NO_THROW(client.increment("noop"));
  EXPECT_NO_THROW(client.gauge("noop", 1.0));
  EXPECT_NO_THROW(client.histogram("noop", 2.0));
  EXPECT_NO_THROW(client.timing("noop", std::chrono::milliseconds{1}));
  EXPECT_NO_THROW(client.set("noop", "value"));
  EXPECT_NO_THROW(client.increment("noop", 1, {}, labels));
  EXPECT_NO_THROW(client.gauge("noop", 1, {}, labels));
  EXPECT_NO_THROW(client.histogram("noop", 2.0, {}, labels));
  EXPECT_NO_THROW(client.timing("noop", std::chrono::milliseconds{1}, {}, labels));
  EXPECT_NO_THROW(client.set("noop", "value", {}, labels));
}

TEST(DogStatsDTest, EmptyNamespace) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "");

  client.increment("requests");
  client.flush();
  EXPECT_EQ(sink.recvMessage(), "requests:1|c");
}

TEST(DogStatsDTest, SocketFails) {
  test::PushSocketAction({-1, error::kTooManyFiles});  // error::kTooManyFiles: too many open files
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
  if (!AERONET_WANT_MALLOC_OVERRIDES) {
    GTEST_SKIP() << "malloc overrides disabled on this toolchain; skipping";
  }
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  const std::string largerThanDatagram(DogStatsD::kMaxDatagramSize, 'm');

  // Only a metric larger than a datagram grows the buffer: make its allocation fail (by malloc or realloc, depending on
  // the build).
  test::FailNextMalloc(1);
  test::FailNextRealloc(1);

  // Should not throw despite allocation failure
  EXPECT_NO_THROW(client.increment(largerThanDatagram));
  test::ResetToSysMalloc();
  test::ResetToSysRealloc();

  client.increment("ok");
  client.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.ok:1|c");
}

TEST(DogStatsDTest, SendSystemError) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");

  // Inject send failure (EBADF - bad file descriptor)
  test::PushSendAction({-1, EBADF});

  // Should not throw despite send() failure
  EXPECT_NO_THROW(client.increment("requests"));
  EXPECT_NO_THROW(client.flush());
}

TEST(DogStatsDTest, SendsAllMetricTypesWithTags) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  DogStatsD::DogStatsDTags tags;
  tags.append("env:dev");
  tags.append("role:web");

  client.increment("hits", 3, tags);
  client.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.hits:3|c|#env:dev,role:web");

  client.gauge("temp", 12, tags);
  client.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.temp:12|g|#env:dev,role:web");

  client.histogram("payload", 4.25, {});
  client.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.payload:4.25|h");

  client.timing("latency", std::chrono::milliseconds{42}, {});
  client.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.latency:42|ms");

  client.set("users", "abc", tags);
  client.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.users:abc|s|#env:dev,role:web");
}

TEST(DogStatsDTest, AppendsPerMeasurementLabels) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  DogStatsD::DogStatsDTags tags;
  tags.append("env:dev");
  const MetricLabel labels[]{
      {"protocol", "h2"},
      {"frame.type", "headers"},
  };

  client.increment("frames", 2, tags, labels);
  client.histogram("ratio", 0.5, {}, labels);
  client.gauge("active", 3, {}, labels);
  client.timing("latency", std::chrono::milliseconds{8}, {}, labels);
  client.set("peers", "client-a", {}, labels);
  client.flush();
  EXPECT_EQ(sink.recvMessage(),
            "svc.frames:2|c|#env:dev,protocol:h2,frame.type:headers\n"
            "svc.ratio:0.5|h|#protocol:h2,frame.type:headers\n"
            "svc.active:3|g|#protocol:h2,frame.type:headers\n"
            "svc.latency:8|ms|#protocol:h2,frame.type:headers\n"
            "svc.peers:client-a|s|#protocol:h2,frame.type:headers");
}

TEST(DogStatsDTest, RespectsExistingNamespaceDotAndEmptyTags) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc.");

  client.increment("requests");
  client.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.requests:1|c");
}

TEST(DogStatsDTest, EmptySocketPathDisablesClient) {
  DogStatsD client({}, "svc");
  EXPECT_NO_THROW(client.increment("noop"));
  EXPECT_NO_THROW(client.gauge("noop", 1.0));
  EXPECT_NO_THROW(client.histogram("noop", 2.0));
  EXPECT_NO_THROW(client.timing("noop", std::chrono::milliseconds{1}));
  EXPECT_NO_THROW(client.set("noop", "value"));
  EXPECT_NO_THROW(client.flush());
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

TEST(DogStatsDTest, NamespaceOfAtMost255CharsWithItsDot) {
  test::UnixDogstatsdSink sink;
  std::string ns(255, 'n');
  EXPECT_THROW(DogStatsD(sink.path(), ns), std::invalid_argument);  // 256 chars with its dot

  ns.back() = '.';
  DogStatsD client(sink.path(), ns);
  EXPECT_EQ(client.ns(), ns);

  ns.pop_back();
  ns.back() = 'm';
  client = DogStatsD(sink.path(), ns);
  EXPECT_EQ(client.ns().size(), 255U);
  EXPECT_EQ(client.ns().back(), '.');
  client.increment("x");
  client.flush();
  EXPECT_EQ(sink.recvMessage(), ns + ".x:1|c");
}

TEST(DogStatsDTest, SendFailureLogsAndContinues) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  sink.closeAndUnlink();
  EXPECT_NO_THROW(client.increment("lost", 1));
  EXPECT_NO_THROW(client.flush());
}

TEST(DogStatsDTest, SendEagainIsDropped) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");

  // Inject EAGAIN for the next send; the client should treat it as a dropped metric
  // and not mark the connection for immediate reconnect. No exception should be thrown.
  test::PushSendAction({-1, error::kWouldBlock});
  EXPECT_NO_THROW(client.increment("lost", 1));
  EXPECT_NO_THROW(client.flush());

  // Subsequent sends should still work (no socket teardown on EAGAIN), without the dropped metrics
  client.increment("ok", 1);
  client.flush();
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
  client.flush();

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
  client.flush();

  // None of the sends should be received by the sink yet.
  std::string msg = sink.recvMessage(100);
  EXPECT_TRUE(msg.empty());

  // next one should trigger a reconnect attempt that will succeed.
  client.increment("requests");
  client.flush();

  // Only the metric recorded once connected is sent.
  msg = sink.recvMessage(500);
  EXPECT_EQ(msg, "svc.requests:1|c");
}

TEST(DogStatsDTest, BuffersMetricsUntilFlush) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");

  client.increment("hits");
  client.gauge("temp", -4);
  client.histogram("payload", 0.5);
  EXPECT_TRUE(sink.recvMessage(50).empty());

  client.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.hits:1|c\nsvc.temp:-4|g\nsvc.payload:0.5|h");

  // Nothing left to send
  client.flush();
  EXPECT_TRUE(sink.recvMessage(50).empty());
}

TEST(DogStatsDTest, SendsDatagramBeforeItExceedsMaxSize) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");

  static constexpr std::string_view kLine = "svc.metric:1|c";
  // Number of newline separated lines fitting in a datagram
  static constexpr std::size_t kNbLinesPerDatagram = (DogStatsD::kMaxDatagramSize + 1U) / (kLine.size() + 1U);
  for (std::size_t lineIdx = 0; lineIdx < kNbLinesPerDatagram; ++lineIdx) {
    client.increment("metric");
  }
  EXPECT_TRUE(sink.recvMessage(50).empty());

  // This one does not fit: the full datagram is sent first
  client.increment("metric");
  const std::string datagram = sink.recvMessage();
  EXPECT_EQ(datagram.size(), (kNbLinesPerDatagram * (kLine.size() + 1U)) - 1U);
  EXPECT_LE(datagram.size(), DogStatsD::kMaxDatagramSize);
  EXPECT_EQ(static_cast<std::size_t>(std::ranges::count(datagram, '\n')) + 1U, kNbLinesPerDatagram);
  EXPECT_TRUE(datagram.starts_with(kLine));
  EXPECT_TRUE(datagram.ends_with(kLine));

  client.flush();
  EXPECT_EQ(sink.recvMessage(), kLine);
}

TEST(DogStatsDTest, SendsMetricLargerThanDatagramAlone) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  const std::string largeName(DogStatsD::kMaxDatagramSize, 'm');

  client.increment("small");
  client.increment(largeName);
  EXPECT_EQ(sink.recvMessage(), "svc.small:1|c");

  client.increment("next");
  EXPECT_EQ(sink.recvMessage(), "svc." + largeName + ":1|c");

  client.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.next:1|c");
}

TEST(DogStatsDTest, DestructorFlushes) {
  test::UnixDogstatsdSink sink;
  {
    DogStatsD client(sink.path(), "svc");
    client.increment("hits", 2);
  }
  EXPECT_EQ(sink.recvMessage(), "svc.hits:2|c");
}

TEST(DogStatsDTest, MoveConstructionKeepsBufferedMetrics) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  client.increment("hits");

  DogStatsD moved(std::move(client));
  EXPECT_TRUE(moved.enabled());
  EXPECT_EQ(moved.socketPath(), sink.path());
  EXPECT_EQ(moved.ns(), "svc.");
  EXPECT_TRUE(sink.recvMessage(50).empty());

  moved.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.hits:1|c");
}

TEST(DogStatsDTest, MoveAssignmentFlushesTheAssignedClient) {
  test::UnixDogstatsdSink sink;
  test::UnixDogstatsdSink otherSink;
  DogStatsD client(sink.path(), "svc");
  DogStatsD other(otherSink.path(), "other");
  client.increment("hits");
  other.increment("misses");

  client = std::move(other);
  EXPECT_EQ(sink.recvMessage(), "svc.hits:1|c");
  EXPECT_EQ(client.ns(), "other.");

  client.flush();
  EXPECT_EQ(otherSink.recvMessage(), "other.misses:1|c");
  EXPECT_TRUE(sink.recvMessage(50).empty());
}

TEST(DogStatsDTest, SelfMoveAssignmentKeepsTheClient) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  client.increment("hits");

  DogStatsD& alias = client;
  client = std::move(alias);
  EXPECT_TRUE(client.enabled());
  EXPECT_EQ(client.ns(), "svc.");
  EXPECT_TRUE(sink.recvMessage(50).empty());

  client.flush();
  EXPECT_EQ(sink.recvMessage(), "svc.hits:1|c");
}

}  // namespace aeronet
