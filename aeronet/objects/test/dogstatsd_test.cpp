#include "aeronet/dogstatsd.hpp"

#include <gtest/gtest.h>
#include <sys/un.h>

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
  // Use an invalid socket path to trigger syscall failure
  EXPECT_THROW(DogStatsD("/invalid/path/to/socket.sock", "svc", std::chrono::milliseconds{5}), std::runtime_error);
}

TEST(DogStatsDTest, SendMetricFailsToAllocateMemory) {
  test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");

  test::FailNextMalloc(1);  // cause next malloc to fail

  // Should not throw despite allocation failure
  EXPECT_NO_THROW(client.increment("requests"));
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
  aeronet::test::UnixDogstatsdSink sink;
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
  std::string ns(256, 'n');
  EXPECT_THROW(DogStatsD({}, ns), std::invalid_argument);
}

TEST(DogStatsDTest, SendFailureLogsAndContinues) {
  aeronet::test::UnixDogstatsdSink sink;
  DogStatsD client(sink.path(), "svc");
  sink.closeAndUnlink();
  EXPECT_NO_THROW(client.increment("lost", 1));
}

}  // namespace aeronet
