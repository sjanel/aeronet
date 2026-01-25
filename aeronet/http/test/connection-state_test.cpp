// Enable syscall overrides for sendfile/pread used in ConnectionState::transportFile
#define AERONET_WANT_SENDFILE_PREAD_OVERRIDES

#include "aeronet/connection-state.hpp"

#include <asm-generic/socket.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/transport.hpp"

using namespace aeronet;
using test::ScopedTempFile;

TEST(ConnectionStateSendfileTest, KernelSendfileSuccess) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(64UL * 1024, 'A');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);
  File file(tmp.filePath().string());
  ConnectionState state;
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  state.fileSend.remaining = content.size();
  state.fileSend.active = true;

  // Ensure the peer socket is blocking (default) so sendfile will make progress.
  auto res = state.transportFile(sv[0], /*tlsFlow=*/false);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::Sent);
  EXPECT_GT(res.bytesDone, 0U);

  // Read the bytes from the other end to verify data flows
  std::string got;
  got.resize(res.bytesDone);
  ssize_t rd = read(sv[1], got.data(), res.bytesDone);
  EXPECT_EQ(rd, static_cast<ssize_t>(res.bytesDone));
}

TEST(ConnectionStateTest, CannotCloseIfOutBufferNotEmpty) {
  ConnectionState state;
  state.closeMode = ConnectionState::CloseMode::DrainThenClose;
  state.outBuffer.append(HttpResponseData{"test"});
  EXPECT_FALSE(state.canCloseConnectionForDrain());
  state.outBuffer.clear();
  state.tunnelOrFileBuffer.append("data");
  EXPECT_FALSE(state.canCloseConnectionForDrain());
  state.tunnelOrFileBuffer.clear();
  EXPECT_TRUE(state.canCloseConnectionForDrain());
}

TEST(ConnectionState, RequestDrainAndCloseHasLowPriority) {
  ConnectionState state;

  state.closeMode = ConnectionState::CloseMode::Immediate;

  state.requestDrainAndClose();
  EXPECT_EQ(state.closeMode, ConnectionState::CloseMode::Immediate);
}

TEST(ConnectionStateSendfileTest, TransportFileInvalidFd) {
  ConnectionState state;
  state.fileSend.file = File();
  state.fileSend.offset = 0;
  state.fileSend.remaining = 1024;
  state.fileSend.active = true;

  auto res = state.transportFile(-1, /*tlsFlow=*/false);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::Error);
  EXPECT_EQ(res.bytesDone, 0);
}

TEST(ConnectionStateSendfileTest, KernelSendfileWouldBlock) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(128UL * 1024, 'B');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  File file(tmp.filePath().string());
  ConnectionState state;
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  state.fileSend.remaining = content.size();
  state.fileSend.active = true;

  // Make client socket non-blocking and intentionally small send buffer so the kernel
  // send buffer fills quickly and sendfile returns EAGAIN.
  int flags = ::fcntl(sv[0], F_GETFL, 0);
  ASSERT_GE(flags, 0);
  ASSERT_EQ(::fcntl(sv[0], F_SETFL, flags | O_NONBLOCK), 0);

  int sndbuf = 1024;  // small
  ASSERT_EQ(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)), 0);

  // Repeatedly call sendFileViaKernel until we observe WouldBlock (EAGAIN) or we
  // exhaust a small iteration budget. On many kernels the initial call may transfer
  // some bytes before the socket send buffer fills, so loop to make the test robust.
  bool sawWouldBlock = false;
  for (int i = 0; i < 32; ++i) {
    auto res = state.transportFile(sv[0], /*tlsFlow=*/false);
    if (res.code == ConnectionState::FileResult::Code::WouldBlock && res.enableWritable) {
      sawWouldBlock = true;
      break;
    }
    if (res.code == ConnectionState::FileResult::Code::Error) {
      FAIL() << "sendFileViaKernel returned Error errno=" << res.bytesDone;
      break;
    }
    // If fully sent, nothing more to do.
    if (res.code == ConnectionState::FileResult::Code::Sent && state.fileSend.remaining == 0) {
      break;
    }
  }
  EXPECT_TRUE(sawWouldBlock);
}

// overrides enabled via macro at file top

TEST(ConnectionStateSendfileTest, KernelSendfileEintrReturnsWouldBlockWithoutEnableWritable) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(4096, 'C');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  File file(tmp.filePath().string());
  ConnectionState state;
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  state.fileSend.remaining = content.size();
  state.fileSend.active = true;

  // Force sendfile to report EINTR once for sv[0]
  test::SetSendfileActions(sv[0], {IoAction{-1, EINTR}});

  auto res = state.transportFile(sv[0], /*tlsFlow=*/false);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::WouldBlock);
  // EINTR should NOT request writable readiness
  EXPECT_FALSE(res.enableWritable);
  // Still active because we haven't transferred anything yet
  EXPECT_TRUE(state.fileSend.active);
}

TEST(ConnectionStateSendfileTest, TlsPreadEintrSetsWouldBlockWhenRemainingPositive) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(1024, 'D');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  File file(tmp.filePath().string());
  ConnectionState state;
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  state.fileSend.remaining = content.size();
  state.fileSend.active = true;

  // Force pread on the file path to return EINTR once
  test::SetPreadPathActions(tmp.filePath().string(), {IoAction{-1, EINTR}});

  auto res = state.transportFile(sv[0], /*tlsFlow=*/true);
  // EINTR with remaining > 0 maps to WouldBlock (retry later) in TLS path
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::WouldBlock);
  // In TLS path, initial enableWritable is true from the FileResult ctor
  EXPECT_TRUE(res.enableWritable);
  EXPECT_TRUE(state.fileSend.active);
}

TEST(ConnectionStateSendfileTest, TlsPreadEintrWithNoRemainingDoesNotSetWouldBlock) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  // Empty file => remaining == 0
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, std::string());

  File file(tmp.filePath().string());
  ConnectionState state;
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  state.fileSend.remaining = 0;
  state.fileSend.active = true;

  // Force pread EINTR; since remaining == 0, code should not flip to WouldBlock per branch
  test::SetPreadPathActions(tmp.filePath().string(), {IoAction{-1, EINTR}});

  auto res = state.transportFile(sv[0], /*tlsFlow=*/true);
  EXPECT_NE(res.code, ConnectionState::FileResult::Code::WouldBlock);
  // It should stay as the initial TLS Read code with 0 bytes
  EXPECT_EQ(res.bytesDone, 0U);
  // Because we returned early on EINTR, active is not cleared here
  EXPECT_TRUE(state.fileSend.active);
}

TEST(ConnectionStateSendfileTest, TlsPreadZeroKeepsActiveWhenRemainingPositive) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(1024, 'Z');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  File file(tmp.filePath().string());
  ConnectionState state;
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  state.fileSend.remaining = content.size();
  state.fileSend.active = true;

  // Simulate pread returning 0 bytes (e.g., unexpected EOF) while remaining > 0
  test::SetPreadPathActions(tmp.filePath().string(), {IoAction{0, 0}});

  auto res = state.transportFile(sv[0], /*tlsFlow=*/true);

  // transportFile should return with 0 bytes read and fileSend should remain active
  EXPECT_EQ(res.bytesDone, 0U);
  EXPECT_TRUE(state.tunnelOrFileBuffer.empty());
  EXPECT_TRUE(state.fileSend.remaining > 0);
  EXPECT_TRUE(state.fileSend.active);
}

TEST(ConnectionStateSendfileTest, TlsSendfileLargeChunks) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  // Create a large file to force multiple chunks in the TLS path
  const std::size_t totalSize = (1 << 20);  // 1 MiB
  const std::string content(totalSize, 'T');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  File file(tmp.filePath().string());
  ConnectionState state;
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  state.fileSend.remaining = content.size();
  state.fileSend.active = true;

  // Attach a PlainTransport that writes to sv[0]. We'll read from sv[1].
  state.transport = std::make_unique<PlainTransport>(sv[0]);

  // Loop until we've consumed the whole file; on each iteration read from file into
  // tunnelOrFileBuffer then write it to the transport and read on the peer socket.
  std::size_t totalRead = 0;
  while (state.fileSend.remaining > 0 || !state.tunnelOrFileBuffer.empty()) {
    // If no pending buffered file data, attempt to read into the buffer (TLS path).
    if (state.tunnelOrFileBuffer.empty() && state.fileSend.remaining > 0) {
      auto fr = state.transportFile(sv[0], /*tlsFlow=*/true);
      EXPECT_NE(fr.code, ConnectionState::FileResult::Code::Error);
      // If WouldBlock is returned (very unlikely for pread), just continue and retry.
      if (fr.code == ConnectionState::FileResult::Code::WouldBlock) {
        continue;
      }
    }

    if (!state.tunnelOrFileBuffer.empty()) {
      // Write buffer via transport
      const auto [written, want] = state.transportWrite(state.tunnelOrFileBuffer);
      EXPECT_NE(want, TransportHint::Error);
      if (written > 0) {
        // Read the bytes from the peer socket to verify
        std::string got;
        got.resize(written);
        ssize_t rd = read(sv[1], got.data(), written);
        EXPECT_EQ(rd, static_cast<ssize_t>(written));
        totalRead += static_cast<std::size_t>(rd);
        state.tunnelOrFileBuffer.erase_front(written);
      } else if (want == TransportHint::WriteReady) {
        // Peer not ready; in this unit test we used blocking sockets and read promptly,
        // so this path is unexpected but safe to break to avoid tight loop.
        break;
      }
    }
  }

  EXPECT_EQ(totalRead, totalSize);
}

TEST(ConnectionStateSendfileTest, KernelSendfileZeroBytesWouldBlock) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  ConnectionState state;
  // Create an empty file to ensure sendfile has nothing to send
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, std::string());
  File file(tmp.filePath().string());
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  // remaining == 0 -> kernel sendfile will likely return 0
  state.fileSend.remaining = 0;
  state.fileSend.active = true;

  auto res = state.transportFile(sv[0], /*tlsFlow=*/false);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::WouldBlock);
  EXPECT_TRUE(res.enableWritable);
}

TEST(ConnectionStateSendfileTest, TlsSendfileEmptyBufferClearsActive) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  ConnectionState state;
  // Empty file to ensure pread returns 0 and no tunnel buffer is filled
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, std::string());
  File file(tmp.filePath().string());
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  state.fileSend.remaining = 0;
  state.fileSend.active = true;

  // Call transportFile in TLS mode which uses pread into tunnelOrFileBuffer
  auto res = state.transportFile(sv[0], /*tlsFlow=*/true);

  EXPECT_EQ(res.bytesDone, 0U);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::Read);

  // After calling with empty file, tunnelOrFileBuffer should be empty and fileSend.active false
  EXPECT_EQ(state.tunnelOrFileBuffer.empty(), true);
  EXPECT_FALSE(state.fileSend.active);
}

TEST(ConnectionStateBufferTest, ShrinkToFitReducesNonEmptyBuffers) {
  ConnectionState state;

  // Grow buffers to have extra capacity
  state.inBuffer.reserve(2048);
  state.inBuffer.append(std::string_view("hello world"));

  state.bodyAndTrailersBuffer.reserve(1025);
  state.bodyAndTrailersBuffer.append(std::string_view("chunked body"));

  state.asyncState.headBuffer.reserve(4096);
  state.asyncState.headBuffer.append(std::string_view("GET / HTTP/1.1\r\nHost: a\r\n\r\n"));

  // Sanity: capacities should be larger than sizes prior to shrink
  EXPECT_GT(state.inBuffer.capacity(), state.inBuffer.size());
  EXPECT_GT(state.bodyAndTrailersBuffer.capacity(), state.bodyAndTrailersBuffer.size());
  EXPECT_GT(state.asyncState.headBuffer.capacity(), state.asyncState.headBuffer.size());

  const auto oldCapacityInBuffer = state.inBuffer.capacity();
  const auto oldCapacityBodyBuffer = state.bodyAndTrailersBuffer.capacity();
  const auto oldCapacityHeadBuffer = state.asyncState.headBuffer.capacity();

  state.reset();

  // After shrink and clear, capacities should be bounded by sizes
  EXPECT_LT(state.inBuffer.capacity(), oldCapacityInBuffer);
  EXPECT_LT(state.bodyAndTrailersBuffer.capacity(), oldCapacityBodyBuffer);
  EXPECT_LT(state.asyncState.headBuffer.capacity(), oldCapacityHeadBuffer);
}

TEST(ConnectionStateBufferTest, ShrinkToFitOnEmptyBuffersYieldsZeroCapacity) {
  ConnectionState state;

  // Ensure buffers are empty
  state.tunnelOrFileBuffer.clear();
  state.inBuffer.clear();
  state.bodyAndTrailersBuffer.clear();
  state.asyncState.headBuffer.clear();

  state.reset();

  // Empty buffers should have capacity 0 after shrink_to_fit
  EXPECT_EQ(state.tunnelOrFileBuffer.capacity(), 0U);
  EXPECT_EQ(state.inBuffer.capacity(), 0U);
  EXPECT_EQ(state.bodyAndTrailersBuffer.capacity(), 0U);
  EXPECT_EQ(state.asyncState.headBuffer.capacity(), 0U);
}

TEST(ConnectionStateBridgeTest, InstallAggregatedBodyBridgeMakesBodyAvailable) {
  ConnectionState state;

  // Populate the request's body (as if it were fully buffered)
  const std::string payload = "aggregated-body-content";
  // Before installing bridge, body access bridge should be null
  EXPECT_FALSE(state.request.isBodyReady());

  // Install bridge which wires request to state.bodyStreamContext
  state.installAggregatedBodyBridge();

  // Provide the buffered body via the public ConnectionState context the bridge will reference
  state.bodyStreamContext.body = payload;
  state.bodyStreamContext.offset = 0;

  // After installing and populating the context, isBodyReady() should return true
  EXPECT_TRUE(state.request.isBodyReady());
  // The aggregate accessor should return the full body
  EXPECT_EQ(state.request.body(), payload);
  // cannot call readBody() after body() on the same request (mutually exclusive modes)
  EXPECT_THROW((void)state.request.readBody(10), std::logic_error);
}

TEST(ConnectionStateBridgeTest, InstallAggregatedBodyBridgeIdempotent) {
  ConnectionState state;

  // Calling installAggregatedBodyBridge twice should be safe (idempotent) and
  // should not crash or change outward behavior.
  state.installAggregatedBodyBridge();

  const std::string payload1 = "first-body";
  state.bodyStreamContext.body = payload1;
  state.bodyStreamContext.offset = 0;
  EXPECT_TRUE(state.request.isBodyReady());
  EXPECT_EQ(state.request.body(), payload1);

  // Modify buffer and call install again; behavior should remain stable.
  const std::string payload2 = "second-body";
  state.bodyStreamContext.body = payload2;
  state.installAggregatedBodyBridge();
  EXPECT_TRUE(state.request.isBodyReady());
  EXPECT_EQ(state.request.body(), payload2);
}

TEST(ConnectionStateBridgeTest, AggregatedBridgeReadWithZeroMaxBytesReturnsEmpty) {
  ConnectionState state;

  // Install the aggregated bridge (this wires request to state.bodyStreamContext)
  state.installAggregatedBodyBridge();

  // Provide the buffered body via the public ConnectionState context the bridge will reference
  const std::string payload = "aggregated-body-content";
  state.bodyStreamContext.body = payload;
  state.bodyStreamContext.offset = 0;

  // Reading with maxBytes == 0 should return empty without advancing the offset
  auto chunk = state.request.readBody(0);
  EXPECT_TRUE(chunk.empty());
  // After the zero-length read, subsequent reads with non-zero should still return data
  auto chunk2 = state.request.readBody(8);
  EXPECT_FALSE(chunk2.empty());
}

TEST(ConnectionStateAsyncStateTest, AsyncHandlerStateClearResetsState) {
  ConnectionState::AsyncHandlerState st;

  // Populate fields to non-default values
  st.awaitReason = ConnectionState::AsyncHandlerState::AwaitReason::WaitingForBody;
  st.active = true;
  st.needsBody = true;
  st.isChunked = true;
  st.expectContinue = true;
  st.consumedBytes = 42;
  st.corsPolicy = reinterpret_cast<const CorsPolicy*>(0x1);
  st.responseMiddleware = reinterpret_cast<const void*>(0x2);
  st.responseMiddlewareCount = 3;
  st.pendingResponse = HttpResponse(http::StatusCodeOK);

  st.clear();

  // All fields should be reset to defaults
  EXPECT_EQ(st.handle, std::coroutine_handle<>());
  EXPECT_EQ(st.awaitReason, ConnectionState::AsyncHandlerState::AwaitReason::None);
  EXPECT_FALSE(st.active);
  EXPECT_FALSE(st.needsBody);
  EXPECT_FALSE(st.isChunked);
  EXPECT_FALSE(st.expectContinue);
  EXPECT_EQ(st.consumedBytes, 0U);
  EXPECT_EQ(st.corsPolicy, nullptr);
  EXPECT_EQ(st.responseMiddleware, nullptr);
  EXPECT_EQ(st.responseMiddlewareCount, 0U);
  EXPECT_FALSE(st.pendingResponse.has_value());
}

TEST(ConnectionStateAsyncStateTest, ClearDestroysNonNullHandle) {
  ConnectionState state;

  // Helper coroutine type that exposes a coroutine handle without destroying it.
  struct HandleBox {
    struct promise_type {
      HandleBox get_return_object() { return HandleBox{std::coroutine_handle<promise_type>::from_promise(*this)}; }
      static std::suspend_always initial_suspend() noexcept { return {}; }
      static std::suspend_never final_suspend() noexcept { return {}; }
      void return_void() {}
      void unhandled_exception() {}
    };

    std::coroutine_handle<promise_type> h;
    explicit HandleBox(std::coroutine_handle<promise_type> hh) : h(hh) {}
    // Release ownership of the handle so caller can transfer it to ConnectionState
    std::coroutine_handle<> release() {
      auto tmp = std::coroutine_handle<>::from_address(h.address());
      h = {};
      return tmp;
    }
  };

  auto make_suspended = []() -> HandleBox { co_return; };

  auto box = make_suspended();
  auto handle = box.release();
  ASSERT_TRUE(handle);

  // Move the handle into state.asyncState and ensure it's present
  state.asyncState.handle = handle;
  EXPECT_TRUE(static_cast<bool>(state.asyncState.handle));

  // clear() should destroy the handle and set it to null
  state.reset();
  EXPECT_EQ(state.asyncState.handle, std::coroutine_handle<>());
}

TEST(ConnectionStateTransportTest, TransportWriteHttpResponseSetsTlsEstablished) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  ConnectionState state;
  // attach a plain transport that writes to sv[0]
  state.transport = std::make_unique<PlainTransport>(sv[0]);

  // ensure tlsEstablished is initially false
  state.tlsEstablished = false;

  HttpResponseData resp("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
  const auto res = state.transportWrite(resp);

  // write should succeed and set tlsEstablished when handshakeDone() is true
  EXPECT_GE(res.bytesProcessed, 0U);
  EXPECT_TRUE(state.tlsEstablished);
}

namespace {
// Simple controllable transport for unit tests
class FakeTransport final : public ITransport {
 public:
  explicit FakeTransport(bool handshakeInitially) : _handshakeDone(handshakeInitially) {}

  TransportResult read(char* buf, std::size_t len) override {
    // Write a small marker and report bytes read
    const char* kMarker = "X";
    if (len > 0) {
      buf[0] = kMarker[0];
      return {1U, TransportHint::None};
    }
    return {0U, TransportHint::None};
  }

  TransportResult write(std::string_view data) override {
    _lastWrite.assign(data.begin(), data.end());
    return {data.size(), TransportHint::None};
  }

  void setHandshakeDone(bool val) { _handshakeDone = val; }

  [[nodiscard]] bool handshakeDone() const noexcept override { return _handshakeDone; }

  [[nodiscard]] std::string lastWrite() const { return _lastWrite; }

 private:
  bool _handshakeDone{true};
  std::string _lastWrite;
};
}  // namespace

TEST(ConnectionStateTransportTest, TransportWriteStringSetsTlsEstablished) {
  ConnectionState state;
  state.transport = std::make_unique<FakeTransport>(/*handshakeInitially=*/true);
  state.tlsEstablished = false;

  const auto res = state.transportWrite(std::string_view{"hello"});
  EXPECT_EQ(res.want, TransportHint::None);
  EXPECT_EQ(res.bytesProcessed, 5U);
  EXPECT_TRUE(state.tlsEstablished);
}

TEST(ConnectionStateTransportTest, TransportWriteStringWaitsUntilHandshakeDone) {
  ConnectionState state;
  auto fake = std::make_unique<FakeTransport>(/*handshakeInitially=*/false);
  FakeTransport* raw = fake.get();
  state.transport = std::move(fake);
  state.tlsEstablished = false;

  // First write: handshake not done yet, tlsEstablished should remain false
  const auto res1 = state.transportWrite(std::string_view{"abc"});
  EXPECT_EQ(res1.want, TransportHint::None);
  EXPECT_EQ(res1.bytesProcessed, 3U);
  EXPECT_FALSE(state.tlsEstablished);

  // Now flip handshake to done and write again; tlsEstablished should become true
  raw->setHandshakeDone(true);
  const auto res2 = state.transportWrite(std::string_view{"def"});
  EXPECT_EQ(res2.want, TransportHint::None);
  EXPECT_EQ(res2.bytesProcessed, 3U);
  EXPECT_TRUE(state.tlsEstablished);
}

TEST(ConnectionStateTransportTest, TransportReadSetsHeaderStartOnce) {
  ConnectionState state;
  state.transport = std::make_unique<FakeTransport>(/*handshakeInitially=*/true);

  // Before any read, headerStartTp is epoch
  EXPECT_EQ(state.headerStartTp.time_since_epoch().count(), 0);

  // First read sets headerStartTp
  auto r1 = state.transportRead(16U);
  EXPECT_EQ(r1.want, TransportHint::None);
  EXPECT_EQ(r1.bytesProcessed, 1U);
  const auto firstTp = state.headerStartTp;
  EXPECT_NE(firstTp.time_since_epoch().count(), 0);

  // Second read should not overwrite the timestamp
  auto r2 = state.transportRead(8U);
  EXPECT_EQ(r2.want, TransportHint::None);
  EXPECT_EQ(r2.bytesProcessed, 1U);
  EXPECT_EQ(state.headerStartTp, firstTp);
}

TEST(ConnectionStateTransportTest, TransportWriteHttpResponseWaitsUntilHandshakeDone) {
  ConnectionState state;
  auto fake = std::make_unique<FakeTransport>(/*handshakeInitially=*/false);
  FakeTransport* raw = fake.get();
  state.transport = std::move(fake);
  state.tlsEstablished = false;

  HttpResponseData resp("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");

  // First write: handshake not done yet, tlsEstablished should remain false
  const auto res1 = state.transportWrite(resp);
  EXPECT_EQ(res1.want, TransportHint::None);
  EXPECT_GE(res1.bytesProcessed, 0U);
  EXPECT_FALSE(state.tlsEstablished);

  // Now flip handshake to done and write again; tlsEstablished should become true
  raw->setHandshakeDone(true);
  const auto res2 = state.transportWrite(resp);
  EXPECT_EQ(res2.want, TransportHint::None);
  EXPECT_GE(res2.bytesProcessed, 0U);
  EXPECT_TRUE(state.tlsEstablished);
}

TEST(ConnectionStateTransportTest, TransportWriteStringSkipsHandshakeWhenAlreadyEstablished) {
  ConnectionState state;
  auto fake = std::make_unique<FakeTransport>(/*handshakeInitially=*/true);
  state.transport = std::move(fake);
  state.tlsEstablished = true;  // simulate prior completion

  const auto res = state.transportWrite(std::string_view{"xyz"});
  EXPECT_EQ(res.want, TransportHint::None);
  EXPECT_EQ(res.bytesProcessed, 3U);
  EXPECT_TRUE(state.tlsEstablished);  // remains true; branch where !tlsEstablished is false
}

TEST(ConnectionStateTransportTest, TransportWriteHttpResponseSkipsHandshakeWhenAlreadyEstablished) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  ConnectionState state;
  state.transport = std::make_unique<PlainTransport>(sv[0]);
  state.tlsEstablished = true;  // simulate prior completion

  HttpResponseData resp("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
  const auto res = state.transportWrite(resp);
  EXPECT_GE(res.bytesProcessed, 0U);
  EXPECT_TRUE(state.tlsEstablished);  // remains true; handshake not re-checked
}

TEST(ConnectionStateSendfileTest, TlsPreadErrorTriggersImmediateCloseAndClearsActive) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(16, 'E');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  File file(tmp.filePath().string());
  ConnectionState state;
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  state.fileSend.remaining = content.size();
  state.fileSend.active = true;

  // Force a hard pread error (not EAGAIN/EINTR) to take default error path
  test::SetPreadPathActions(tmp.filePath().string(), {IoAction{-1, EIO}});

  auto res = state.transportFile(sv[0], /*tlsFlow=*/true);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::Error);
  EXPECT_EQ(res.bytesDone, 0U);
  EXPECT_FALSE(state.fileSend.active);
  EXPECT_TRUE(state.isImmediateCloseRequested());
}

TEST(ConnectionStateTest, AttachFilePayloadMustReturnFalseIfNoFile) {
  ConnectionState state;

  state.outBuffer = HttpResponseData{"response data"};

  // With file attached, attachFilePayload should return false for non-empty output buffer
  EXPECT_FALSE(state.attachFilePayload(FilePayload{File{}, 2, 4}));
}
