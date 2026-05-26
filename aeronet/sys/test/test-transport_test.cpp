#include "aeronet/test-transport.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "aeronet/fault-policy.hpp"
#include "aeronet/test-pipe.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::test {
namespace {

// --- TestPipe basic tests ---

TEST(TestPipe, PushAndPullBasic) {
  TestPipe pipe;
  pipe.pushToServer("hello");
  EXPECT_EQ(pipe.serverReadAvailable(), 5U);

  auto data = pipe.serverRead(5);
  EXPECT_EQ(data, "hello");
  pipe.serverConsume(5);
  EXPECT_EQ(pipe.serverReadAvailable(), 0U);
}

TEST(TestPipe, ServerWriteAndClientPull) {
  TestPipe pipe;
  pipe.serverWrite("response");
  EXPECT_EQ(pipe.clientReadAvailable(), 8U);

  auto pulled = pipe.pullFromServer();
  EXPECT_EQ(pulled, "response");
  EXPECT_EQ(pipe.clientReadAvailable(), 0U);
}

TEST(TestPipe, PartialServerRead) {
  TestPipe pipe;
  pipe.pushToServer("abcdefgh");

  auto data = pipe.serverRead(3);
  EXPECT_EQ(data, "abc");
  pipe.serverConsume(3);

  data = pipe.serverRead(10);
  EXPECT_EQ(data, "defgh");
}

TEST(TestPipe, PullFromServerWithMaxBytes) {
  TestPipe pipe;
  pipe.serverWrite("long response data");

  auto partial = pipe.pullFromServer(4);
  EXPECT_EQ(partial, "long");
  EXPECT_EQ(pipe.clientReadAvailable(), 14U);
}

TEST(TestPipe, EmptyReads) {
  TestPipe pipe;
  EXPECT_EQ(pipe.serverReadAvailable(), 0U);
  EXPECT_EQ(pipe.clientReadAvailable(), 0U);

  auto data = pipe.serverRead(10);
  EXPECT_TRUE(data.empty());

  auto pulled = pipe.pullFromServer();
  EXPECT_TRUE(pulled.empty());
}

TEST(TestPipe, CloseClientEnd) {
  TestPipe pipe;
  pipe.pushToServer("data");
  pipe.closeClientEnd();

  EXPECT_TRUE(pipe.isClientClosed());
  EXPECT_FALSE(pipe.isServerClosed());
  // Data pushed before close is still available
  EXPECT_EQ(pipe.serverReadAvailable(), 4U);
}

TEST(TestPipe, ResetClientEnd) {
  TestPipe pipe;
  pipe.resetClientEnd();
  EXPECT_TRUE(pipe.isClientReset());
  EXPECT_FALSE(pipe.isServerReset());
}

// --- TestTransport basic tests ---

TEST(TestTransport, ReadFromPipeNoFaults) {
  auto [transport, pipe] = MakeTestTransport();
  pipe.pushToServer("GET / HTTP/1.1\r\n");

  char buf[64]{};
  auto result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 16U);
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_EQ(std::string_view(buf, 16), "GET / HTTP/1.1\r\n");
}

TEST(TestTransport, WriteToTransportNoFaults) {
  auto [transport, pipe] = MakeTestTransport();

  std::string_view response = "HTTP/1.1 200 OK\r\n\r\n";
  auto result = transport->write(response);
  EXPECT_EQ(result.bytesProcessed, response.size());
  EXPECT_EQ(result.want, TransportHint::None);

  auto pulled = pipe.pullFromServer();
  EXPECT_EQ(pulled, response);
}

TEST(TestTransport, ReadWhenEmptyReturnsReadReady) {
  auto [transport, pipe] = MakeTestTransport();

  char buf[16]{};
  auto result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::ReadReady);
}

TEST(TestTransport, ReadAfterClientCloseReturnsEof) {
  auto [transport, pipe] = MakeTestTransport();
  pipe.closeClientEnd();

  char buf[16]{};
  auto result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::None);  // orderly close
}

TEST(TestTransport, ReadAfterClientResetReturnsError) {
  auto [transport, pipe] = MakeTestTransport();
  pipe.resetClientEnd();

  char buf[16]{};
  auto result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::Error);
}

// --- Partial read fault injection ---

TEST(TestTransport, PartialReadsCappedByMaxBytes) {
  FaultPolicy policy;
  policy.maxBytesPerRead = 3;

  auto [transport, pipe] = MakeTestTransport(policy);
  pipe.pushToServer("Hello, World!");

  char buf[64]{};
  auto result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 3U);
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_EQ(std::string_view(buf, 3), "Hel");

  // Second read gets next 3 bytes
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 3U);
  EXPECT_EQ(std::string_view(buf, 3), "lo,");
}

TEST(TestTransport, PartialWritesCappedByMaxBytes) {
  FaultPolicy policy;
  policy.maxBytesPerWrite = 4;

  auto [transport, pipe] = MakeTestTransport(policy);

  std::string_view data = "Hello, World!";
  auto result = transport->write(data);
  EXPECT_EQ(result.bytesProcessed, 4U);
  EXPECT_EQ(result.want, TransportHint::None);

  result = transport->write(data.substr(4));
  EXPECT_EQ(result.bytesProcessed, 4U);

  result = transport->write(data.substr(8));
  EXPECT_EQ(result.bytesProcessed, 4U);

  result = transport->write(data.substr(12));
  EXPECT_EQ(result.bytesProcessed, 1U);

  auto pulled = pipe.pullFromServer();
  EXPECT_EQ(pulled, "Hello, World!");
}

// --- EAGAIN simulation ---

TEST(TestTransport, EagainEveryNReads) {
  FaultPolicy policy;
  policy.eagainAfterEveryNReads = 2;  // EAGAIN on read #2, #4, #6...

  auto [transport, pipe] = MakeTestTransport(policy);
  pipe.pushToServer("abcdef");

  char buf[64]{};

  // Read #1: succeeds (consumes all available)
  auto result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_EQ(result.bytesProcessed, 6U);

  // Push more data for subsequent reads
  pipe.pushToServer("ghijkl");

  // Read #2: EAGAIN (call count is 2, 2 % 2 == 0)
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::ReadReady);

  // Read #3: succeeds
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_EQ(result.bytesProcessed, 6U);
  EXPECT_EQ(std::string_view(buf, 6), "ghijkl");
}

TEST(TestTransport, EagainEveryNWrites) {
  FaultPolicy policy;
  policy.eagainAfterEveryNWrites = 2;  // EAGAIN on write #2, #4...

  auto [transport, pipe] = MakeTestTransport(policy);

  // Write #1: succeeds
  auto result = transport->write("abc");
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_EQ(result.bytesProcessed, 3U);

  // Write #2: EAGAIN
  result = transport->write("def");
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::WriteReady);

  // Write #3: succeeds
  result = transport->write("def");
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_EQ(result.bytesProcessed, 3U);

  auto pulled = pipe.pullFromServer();
  EXPECT_EQ(pulled, "abcdef");
}

// --- Connection reset mid-stream ---

TEST(TestTransport, ResetAfterTotalBytesRead) {
  FaultPolicy policy;
  policy.resetAfterTotalBytesRead = 10;

  auto [transport, pipe] = MakeTestTransport(policy);
  pipe.pushToServer("12345678901234567890");

  char buf[64]{};

  // First read: up to 10 bytes
  auto result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 10U);
  EXPECT_EQ(result.want, TransportHint::None);

  // Second read: error (threshold reached)
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::Error);
}

TEST(TestTransport, ResetAfterTotalBytesWritten) {
  FaultPolicy policy;
  policy.resetAfterTotalBytesWritten = 5;

  auto [transport, pipe] = MakeTestTransport(policy);

  auto result = transport->write("abc");
  EXPECT_EQ(result.bytesProcessed, 3U);
  EXPECT_EQ(result.want, TransportHint::None);

  // Next write: only 2 more bytes allowed
  result = transport->write("defgh");
  EXPECT_EQ(result.bytesProcessed, 2U);
  EXPECT_EQ(result.want, TransportHint::None);

  // Threshold reached: error
  result = transport->write("x");
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::Error);
}

TEST(TestTransport, ResetOnNextReadOneShot) {
  auto [transport, pipe] = MakeTestTransport();
  pipe.pushToServer("data");

  transport->faultPolicy().resetOnNextRead = true;

  char buf[16]{};
  auto result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::Error);

  // One-shot: next read succeeds (flag auto-cleared)
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_EQ(result.bytesProcessed, 4U);
}

TEST(TestTransport, ResetOnNextWriteOneShot) {
  auto [transport, pipe] = MakeTestTransport();

  transport->faultPolicy().resetOnNextWrite = true;

  auto result = transport->write("data");
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::Error);

  // One-shot: next write succeeds
  result = transport->write("data");
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_EQ(result.bytesProcessed, 4U);
}

// --- Randomized partial reads with seed ---

TEST(TestTransport, SeededPartialReadsAreDeterministic) {
  FaultPolicy policy;
  policy.maxBytesPerRead = 5;
  policy.seed = 42;

  // Run twice with same seed and same data — should get identical byte counts
  vector<std::size_t> run1;
  vector<std::size_t> run2;

  for (int run = 0; run < 2; ++run) {
    auto [transport, pipe] = MakeTestTransport(policy);
    pipe.pushToServer("abcdefghijklmnopqrstuvwxyz");

    auto& results = (run == 0) ? run1 : run2;
    char buf[64]{};
    std::size_t total = 0;
    while (total < 26) {
      auto result = transport->read(buf, sizeof(buf));
      if (result.bytesProcessed == 0) {
        break;
      }
      results.push_back(result.bytesProcessed);
      total += result.bytesProcessed;
    }
  }

  ASSERT_EQ(run1.size(), run2.size());
  for (uint32_t idx = 0; idx < run1.size(); ++idx) {
    EXPECT_EQ(run1[idx], run2[idx]) << "Mismatch at read #" << idx;
  }
}

// --- Counters ---

TEST(TestTransport, TracksTotalBytesAndCallCounts) {
  auto [transport, pipe] = MakeTestTransport();
  pipe.pushToServer("hello");

  char buf[16]{};
  transport->read(buf, sizeof(buf));
  transport->write("world");

  EXPECT_EQ(transport->totalBytesRead(), 5U);
  EXPECT_EQ(transport->totalBytesWritten(), 5U);
  EXPECT_EQ(transport->readCallCount(), 1U);
  EXPECT_EQ(transport->writeCallCount(), 1U);
}

// --- Handshake state ---

TEST(TestTransport, HandshakeStateControllable) {
  auto [transport, pipe] = MakeTestTransport();
  EXPECT_TRUE(transport->handshakeDone());

  transport->setHandshakeDone(false);
  EXPECT_FALSE(transport->handshakeDone());

  transport->setHandshakeDone(true);
  EXPECT_TRUE(transport->handshakeDone());
}

// --- hasPendingReadData ---

TEST(TestTransport, HasPendingReadDataReflectsPipeState) {
  auto [transport, pipe] = MakeTestTransport();
  EXPECT_FALSE(transport->hasPendingReadData());

  pipe.pushToServer("data");
  EXPECT_TRUE(transport->hasPendingReadData());

  char buf[16]{};
  transport->read(buf, sizeof(buf));
  EXPECT_FALSE(transport->hasPendingReadData());
}

// --- Combined faults ---

TEST(TestTransport, PartialReadsWithEagainCombined) {
  FaultPolicy policy;
  policy.maxBytesPerRead = 2;
  policy.eagainAfterEveryNReads = 3;  // EAGAIN on read #3, #6, #9...

  auto [transport, pipe] = MakeTestTransport(policy);
  pipe.pushToServer("abcdefghij");

  char buf[16]{};
  std::string assembled;

  // Read #1: 2 bytes
  auto result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 2U);
  assembled.append(buf, result.bytesProcessed);

  // Read #2: 2 bytes
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 2U);
  assembled.append(buf, result.bytesProcessed);

  // Read #3: EAGAIN
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::ReadReady);

  // Read #4: 2 bytes
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 2U);
  assembled.append(buf, result.bytesProcessed);

  // Read #5: 2 bytes
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 2U);
  assembled.append(buf, result.bytesProcessed);

  // Read #6: EAGAIN
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::ReadReady);

  // Read #7: 2 bytes (remaining)
  result = transport->read(buf, sizeof(buf));
  EXPECT_EQ(result.bytesProcessed, 2U);
  assembled.append(buf, result.bytesProcessed);

  EXPECT_EQ(assembled, "abcdefghij");
}

TEST(TestTransport, WriteAfterClientCloseReturnsError) {
  auto [transport, pipe] = MakeTestTransport();
  pipe.closeClientEnd();

  auto result = transport->write("data");
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::Error);
}

}  // namespace
}  // namespace aeronet::test
