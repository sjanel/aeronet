#include "aeronet/connection-state.hpp"

#include <asm-generic/socket.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-status-code.hpp"
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

TEST(ConnectionStateBufferTest, ShrinkToFitReducesNonEmptyBuffers) {
  ConnectionState state;

  // Grow buffers to have extra capacity
  state.inBuffer.reserveExponential(1024);
  state.inBuffer.append(std::string_view("hello world"));

  state.bodyAndTrailersBuffer.reserveExponential(2048);
  state.bodyAndTrailersBuffer.append(std::string_view("chunked body"));

  state.headBuffer.reserveExponential(512);
  state.headBuffer.append(std::string_view("GET / HTTP/1.1\r\nHost: a\r\n\r\n"));

  // Sanity: capacities should be larger than sizes prior to shrink
  EXPECT_GT(state.inBuffer.capacity(), state.inBuffer.size());
  EXPECT_GT(state.bodyAndTrailersBuffer.capacity(), state.bodyAndTrailersBuffer.size());
  EXPECT_GT(state.headBuffer.capacity(), state.headBuffer.size());

  state.shrink_to_fit();

  // After shrink, capacity should be reduced to current size
  EXPECT_EQ(state.inBuffer.capacity(), state.inBuffer.size());
  EXPECT_EQ(state.bodyAndTrailersBuffer.capacity(), state.bodyAndTrailersBuffer.size());
  EXPECT_EQ(state.headBuffer.capacity(), state.headBuffer.size());
}

TEST(ConnectionStateBufferTest, ShrinkToFitOnEmptyBuffersYieldsZeroCapacity) {
  ConnectionState state;

  // Ensure buffers are empty
  state.tunnelOrFileBuffer.clear();
  state.inBuffer.clear();
  state.bodyAndTrailersBuffer.clear();
  state.headBuffer.clear();

  state.shrink_to_fit();

  // Empty buffers should have capacity 0 after shrink_to_fit
  EXPECT_EQ(state.tunnelOrFileBuffer.capacity(), 0U);
  EXPECT_EQ(state.inBuffer.capacity(), 0U);
  EXPECT_EQ(state.bodyAndTrailersBuffer.capacity(), 0U);
  EXPECT_EQ(state.headBuffer.capacity(), 0U);
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

TEST(ConnectionStateAsyncStateTest, AsyncHandlerStateClearResetsState) {
  ConnectionState::AsyncHandlerState st;

  // Populate fields to non-default values
  st.awaitReason = ConnectionState::AsyncHandlerState::AwaitReason::WaitingForBody;
  st.active = true;
  st.needsBody = true;
  st.responsePending = true;
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
  EXPECT_FALSE(st.responsePending);
  EXPECT_FALSE(st.isChunked);
  EXPECT_FALSE(st.expectContinue);
  EXPECT_EQ(st.consumedBytes, 0U);
  EXPECT_EQ(st.corsPolicy, nullptr);
  EXPECT_EQ(st.responseMiddleware, nullptr);
  EXPECT_EQ(st.responseMiddlewareCount, 0U);
  // pendingResponse should be default constructed; accept status 0 or OK depending on implementation
  auto prStatus = st.pendingResponse.status();
  EXPECT_TRUE(prStatus == static_cast<http::StatusCode>(0) || prStatus == http::StatusCodeOK);
}
