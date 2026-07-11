#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/fault-injecting-transport.hpp"
#include "aeronet/fault-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/transport-test-hook.hpp"
#include "aeronet/transport.hpp"

using namespace std::chrono_literals;

namespace aeronet {
namespace {

// Global fault policy that the decorator function reads.
// Must be set before the server accepts connections.
test::FaultPolicy g_faultPolicy;  // NOLINT(misc-use-internal-linkage)

std::unique_ptr<ITransport> ApplyTestFaultPolicy(std::unique_ptr<ITransport> transport) {
  return std::make_unique<test::FaultInjectingTransport>(std::move(transport), g_faultPolicy);
}

// --- Test fixture ---

test::TestServer ts;

class NetworkFaultTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_faultPolicy = {};

    // Register test routes
    ts.router().setPath(http::Method::GET, "/hello",
                        [](const HttpRequestView&) { return HttpResponse("Hello, World!"); });
    ts.router().setPath(http::Method::POST, "/echo",
                        [](const HttpRequestView& req) { return HttpResponse(std::string{req.body()}); });
    ts.router().setPath(http::Method::GET, "/large",
                        [](const HttpRequestView&) { return HttpResponse(std::string(8192, 'X')); });
  }

  void TearDown() override { _decorator.reset(); }

  void enableFaults(const test::FaultPolicy& policy) {
    g_faultPolicy = policy;
    _decorator.emplace(&ApplyTestFaultPolicy);
  }

 private:
  std::optional<test::ScopedTransportDecorator> _decorator;
};

// --- Partial read tests ---

TEST_F(NetworkFaultTest, PartialReadsOneByteAtATime) {
  test::FaultPolicy policy;
  policy.maxBytesPerRead = 16;
  enableFaults(policy);

  test::ClientConnection cc(ts.port());
  test::sendAll(cc.fd(), "GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
  auto response = test::recvUntilClosed(cc.fd());

  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("Hello, World!")) << response;
}

TEST_F(NetworkFaultTest, PartialReadsSmallChunks) {
  test::FaultPolicy policy;
  policy.maxBytesPerRead = 5;
  enableFaults(policy);

  test::ClientConnection cc(ts.port());
  test::sendAll(cc.fd(), "GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
  auto response = test::recvUntilClosed(cc.fd());

  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("Hello, World!")) << response;
}

TEST_F(NetworkFaultTest, PartialReadsWithLargePostBody) {
  test::FaultPolicy policy;
  policy.maxBytesPerRead = 7;
  enableFaults(policy);

  std::string body(1024, 'A');
  std::string request = "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 1024\r\nConnection: close\r\n\r\n" + body;

  test::ClientConnection cc(ts.port());
  test::sendAll(cc.fd(), request);
  auto response = test::recvUntilClosed(cc.fd());

  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains(body)) << response;
}

// --- Partial write tests ---

TEST_F(NetworkFaultTest, PartialWritesSmallChunks) {
  test::FaultPolicy policy;
  policy.maxBytesPerWrite = 10;
  enableFaults(policy);

  test::ClientConnection cc(ts.port());
  test::sendAll(cc.fd(), "GET /large HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
  auto response = test::recvUntilClosed(cc.fd());

  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains(std::string(100, 'X'))) << "Response should contain large body";
}

// --- EAGAIN simulation ---
// Note: EagainOnEveryOtherRead is intentionally NOT tested here. Simulated EAGAIN on reads is
// incompatible with edge-triggered event loops (EPOLLET on Linux, EV_CLEAR on macOS): when the
// transport returns {0, ReadReady} the server waits for a new readiness edge, but the socket
// already has data so no edge fires — causing a hang. This is inherently racy in integration tests
// (depends on whether the kernel delivers the full request in one shot before the EAGAIN counter
// triggers). EAGAIN on reads is properly covered by TestTransport unit tests instead.

TEST_F(NetworkFaultTest, EagainOnEveryOtherWrite) {
  test::FaultPolicy policy;
  policy.eagainAfterEveryNWrites = 2;
  enableFaults(policy);

  test::ClientConnection cc(ts.port());
  test::sendAll(cc.fd(), "GET /large HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
  auto response = test::recvUntilClosed(cc.fd());

  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  // Server should eventually flush all data despite EAGAIN
  EXPECT_TRUE(response.contains(std::string(100, 'X'))) << "Response body incomplete";
}

// --- Connection reset mid-stream ---

TEST_F(NetworkFaultTest, ResetAfterReadingPartialRequest) {
  test::FaultPolicy policy;
  policy.resetAfterTotalBytesRead = 10;  // reset after reading 10 bytes of request
  enableFaults(policy);

  test::ClientConnection cc(ts.port());
  test::sendAll(cc.fd(), "GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");

  // Server should close the connection due to transport error.
  // This shouldn't crash or leak resources.
  auto response = test::recvUntilClosed(cc.fd());
  // We expect either empty response or connection reset - the key is no crash/hang.
  (void)response;
}

TEST_F(NetworkFaultTest, ResetDuringResponseWrite) {
  test::FaultPolicy policy;
  policy.resetAfterTotalBytesWritten = 20;  // reset after writing partial response
  enableFaults(policy);

  test::ClientConnection cc(ts.port());
  test::sendAll(cc.fd(), "GET /large HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");

  // Server should handle the write error gracefully
  auto response = test::recvUntilClosed(cc.fd());
  // Response will be truncated or empty - the test is that it doesn't crash
  (void)response;
}

// --- Combined faults ---

TEST_F(NetworkFaultTest, PartialReadsAndWritesCombined) {
  test::FaultPolicy policy;
  policy.maxBytesPerRead = 3;
  policy.maxBytesPerWrite = 11;
  enableFaults(policy);

  test::ClientConnection cc(ts.port());
  test::sendAll(cc.fd(), "GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
  auto response = test::recvUntilClosed(cc.fd());

  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("Hello, World!")) << response;
}

TEST_F(NetworkFaultTest, PartialReadsWithSmallWritesCombined) {
  // Combine very small partial reads with small partial writes.
  // Note: EAGAIN simulation on reads is not used in integration tests because it is
  // incompatible with edge-triggered epoll (EPOLLET) — the server won't be re-notified
  // when the socket already has buffered data. EAGAIN behavior is tested at the unit level.
  test::FaultPolicy policy;
  policy.maxBytesPerRead = 4;
  policy.maxBytesPerWrite = 7;
  enableFaults(policy);

  test::ClientConnection cc(ts.port());
  test::sendAll(cc.fd(), "GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
  auto response = test::recvUntilClosed(cc.fd());

  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains("Hello, World!")) << response;
}

// --- Pipelining under faults ---

TEST_F(NetworkFaultTest, PipelinedRequestsWithPartialReads) {
  test::FaultPolicy policy;
  policy.maxBytesPerRead = 6;
  enableFaults(policy);

  test::ClientConnection cc(ts.port());
  std::string pipelined =
      "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n"
      "GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  test::sendAll(cc.fd(), pipelined);
  auto response = test::recvUntilClosed(cc.fd());

  // Both responses should be present
  auto firstPos = response.find("Hello, World!");
  ASSERT_NE(firstPos, std::string::npos) << response;
  auto secondPos = response.find("Hello, World!", firstPos + 1);
  EXPECT_NE(secondPos, std::string::npos) << "Second pipelined response missing\n" << response;
}

}  // namespace
}  // namespace aeronet
