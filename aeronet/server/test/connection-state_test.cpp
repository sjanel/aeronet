// Enable syscall overrides for sendfile/pread used in ConnectionState::transportFile, and for the sendmsg /
// recvmsg(MSG_ERRQUEUE) of the zerocopy sends
#include "aeronet/system-error.hpp"
#define AERONET_WANT_SENDFILE_PREAD_OVERRIDES
#define AERONET_WANT_READ_WRITE_OVERRIDES
#define AERONET_WANT_SOCKET_OVERRIDES

#include "aeronet/connection-state.hpp"

#ifdef AERONET_POSIX
#include <fcntl.h>
#endif
#include <gtest/gtest.h>
#ifdef AERONET_LINUX
#include <linux/errqueue.h>
#endif
#ifdef AERONET_POSIX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/output-queue.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy-buffers.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "aeronet/zerocopy.hpp"
#include "message-data-test-helpers.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/async-handler-state.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/request-task.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>

#include "aeronet/tls-transport.hpp"
#endif

using namespace aeronet;
using test::ScopedTempFile;

TEST(ConnectionStateSendfileTest, KernelSendfileSuccess) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(64UL * 1024, 'A');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  ConnectionState state;
  state.fileSend.filePayload = FilePayload(File(tmp.filePath().string()), 0, content.size());
  state.fileSendActive = true;

  // Ensure the peer socket is blocking (default) so sendfile will make progress.
  auto res = state.transportFile(sv[0], /*tlsFlow=*/false);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::Sent);
  EXPECT_GT(res.bytesDone, 0U);

  // Read the bytes from the other end to verify data flows
  std::string got;
  got.resize(res.bytesDone);
  auto rd = read(sv[1], got.data(), res.bytesDone);
  ASSERT_GE(rd, 0);
  EXPECT_EQ(static_cast<decltype(res.bytesDone)>(rd), res.bytesDone);
}

TEST(ConnectionStateTest, CannotCloseWithPendingOutput) {
  ConnectionState state;
  state.closeMode = ConnectionState::CloseMode::DrainThenClose;
  state.queueOutput(HttpMessageData{RawChars("test")});
  EXPECT_FALSE(state.canCloseConnectionForDrain());
  state.outputWritten(4, /*zerocopySent=*/false);
  state.tunnelOrFileBuffer.append("data");
  EXPECT_FALSE(state.canCloseConnectionForDrain());
  state.tunnelOrFileBuffer.clear();
  EXPECT_TRUE(state.canCloseConnectionForDrain());
}

TEST(ConnectionStateTest, InBufferIsNotShrunkWhileTheConnectTargetIsResolved) {
  static constexpr std::string_view kConnectRequest = "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n";
  ConnectionState state;
  // The first read reserves a large buffer for a short request.
  state.inBuffer.reserve(128UL * 1024UL);
  state.inBuffer.append(kConnectRequest);
  const char* storage = state.inBuffer.data();
  const auto capacity = state.inBuffer.capacity();

  // The request views point into the inBuffer until the CONNECT is answered.
  state.tunnelResolving = true;
  state.reclaimMemoryFromOversizedBuffers();
  EXPECT_EQ(state.inBuffer.data(), storage);
  EXPECT_EQ(state.inBuffer.capacity(), capacity);

  state.tunnelResolving = false;
  state.reclaimMemoryFromOversizedBuffers();
  EXPECT_LT(state.inBuffer.capacity(), capacity);
  EXPECT_EQ(std::string_view(state.inBuffer.data(), state.inBuffer.size()), kConnectRequest);
}

TEST(ConnectionState, RequestDrainAndCloseIsIdempotent) {
  ConnectionState state;

  state.closeMode = ConnectionState::CloseMode::DrainThenClose;

  state.requestDrainAndClose();
  EXPECT_EQ(state.closeMode, ConnectionState::CloseMode::DrainThenClose);
}

TEST(ConnectionStateSendfileTest, TransportFileInvalidFd) {
  ConnectionState state;
  state.fileSend.filePayload = FilePayload(File{}, 0, 1024);
  state.fileSendActive = true;
  // Output queued behind the file: it must not follow its truncated body.
  EXPECT_FALSE(state.queueOutput(test::MakeMessageData(16, 'q')));

  auto res = state.transportFile(-1, /*tlsFlow=*/false);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::Error);
  EXPECT_EQ(res.bytesDone, 0);
  EXPECT_FALSE(state.isSendingFile());
  EXPECT_FALSE(state.hasPendingOutput());
  EXPECT_TRUE(state.isAnyCloseRequested());
}

TEST(ConnectionStateSendfileTest, KernelSendfileWouldBlock) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(128UL * 1024, 'B');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  ConnectionState state;
  state.fileSend.filePayload = FilePayload(File(tmp.filePath().string()), 0, content.size());
  state.fileSendActive = true;

  // Make client socket non-blocking and intentionally small send buffer so the kernel
  // send buffer fills quickly and sendfile returns EAGAIN.
  int flags = ::fcntl(sv[0], F_GETFL, 0);
  ASSERT_GE(flags, 0);

  // NOLINTNEXTLINE(bugprone-signed-bitwise)
  ASSERT_EQ(::fcntl(sv[0], F_SETFL, flags | O_NONBLOCK), 0);

  int sndbuf = 1024;  // small
  // NOLINTNEXTLINE(misc-include-cleaner) sys/socket.h is the correct header for SOL_SOCKET and SO_SNDBUF
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
    if (res.code == ConnectionState::FileResult::Code::Sent && state.fileSend.filePayload.length == 0) {
      break;
    }
  }
  EXPECT_TRUE(sawWouldBlock);
}

// overrides enabled via macro at file top

TEST(ConnectionStateSendfileTest, KernelSendfileEintrReturnsWouldBlockWithoutEnableWritable) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(4096, 'C');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  ConnectionState state;
  state.fileSend.filePayload = FilePayload(File(tmp.filePath().string()), 0, content.size());
  state.fileSendActive = true;

  // Force sendfile to report error::kInterrupted once for sv[0]
  test::SetSendfileActions(sv[0], {IoAction{-1, error::kInterrupted}});

  auto res = state.transportFile(sv[0], /*tlsFlow=*/false);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::WouldBlock);
  // error::kInterrupted should NOT request writable readiness
  EXPECT_FALSE(res.enableWritable);
  // Still active because we haven't transferred anything yet
  EXPECT_TRUE(state.fileSendActive);
}

TEST(ConnectionStateSendfileTest, TlsPreadEintrSetsWouldBlockWhenRemainingPositive) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(1024, 'D');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  ConnectionState state;
  state.fileSend.filePayload = FilePayload(File(tmp.filePath().string()), 0, content.size());
  state.fileSendActive = true;

  // Force pread on the file path to return error::kInterrupted once
  test::SetPreadPathActions(tmp.filePath().string(), {IoAction{-1, error::kInterrupted}});

  auto res = state.transportFile(sv[0], /*tlsFlow=*/true);
  // error::kInterrupted with remaining > 0 maps to WouldBlock (retry later) in TLS path
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::WouldBlock);
  // In TLS path, initial enableWritable is true from the FileResult ctor
  EXPECT_TRUE(res.enableWritable);
  EXPECT_TRUE(state.fileSendActive);
}

TEST(ConnectionStateSendfileTest, TlsPreadEintrWithNoRemainingDoesNotSetWouldBlock) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  // Empty file => remaining == 0
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, std::string());

  ConnectionState state;
  state.fileSend.filePayload = FilePayload(File(tmp.filePath().string()), 0, 0);
  state.fileSendActive = true;

  // Force pread error::kInterrupted; since remaining == 0, code should not flip to WouldBlock per branch
  test::SetPreadPathActions(tmp.filePath().string(), {IoAction{-1, error::kInterrupted}});

  auto res = state.transportFile(sv[0], /*tlsFlow=*/true);
  EXPECT_NE(res.code, ConnectionState::FileResult::Code::WouldBlock);
  // It should stay as the initial TLS Read code with 0 bytes
  EXPECT_EQ(res.bytesDone, 0U);
  // Because we returned early on error::kInterrupted, active is not cleared here
  EXPECT_TRUE(state.fileSendActive);
}

TEST(ConnectionStateSendfileTest, TlsPreadZeroKeepsActiveWhenRemainingPositive) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(1024, 'Z');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  ConnectionState state;
  state.fileSend.filePayload = FilePayload(File(tmp.filePath().string()), 0, content.size());
  state.fileSendActive = true;

  // Simulate pread returning 0 bytes (e.g., unexpected EOF) while remaining > 0
  test::SetPreadPathActions(tmp.filePath().string(), {IoAction{0, 0}});

  auto res = state.transportFile(sv[0], /*tlsFlow=*/true);

  // transportFile should return with 0 bytes read and fileSend should remain active
  EXPECT_EQ(res.bytesDone, 0U);
  EXPECT_TRUE(state.tunnelOrFileBuffer.empty());
  EXPECT_TRUE(state.fileSend.filePayload.length > 0);
  EXPECT_TRUE(state.fileSendActive);
}

TEST(ConnectionStateSendfileTest, TlsSendfileLargeChunks) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  // Create a large file to force multiple chunks in the TLS path
  const std::size_t totalSize = 1UL << 20U;  // 1 MiB
  const std::string content(totalSize, 'T');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  ConnectionState state;
  state.fileSend.filePayload = FilePayload(File(tmp.filePath().string()), 0, content.size());
  state.fileSendActive = true;

  // Attach a PlainTransport that writes to sv[0]. We'll read from sv[1].
  state.transport = Transport(sv[0]);

  // Loop until we've consumed the whole file; on each iteration read from file into
  // tunnelOrFileBuffer then write it to the transport and read on the peer socket.
  std::size_t totalRead = 0;
  while (state.fileSend.filePayload.length > 0 || !state.tunnelOrFileBuffer.empty()) {
    // If no pending buffered file data, attempt to read into the buffer (TLS path).
    if (state.tunnelOrFileBuffer.empty() && state.fileSend.filePayload.length > 0) {
      const auto fr = state.transportFile(sv[0], /*tlsFlow=*/true);
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
        const auto rd = read(sv[1], got.data(), written);
        ASSERT_GE(rd, 0);
        EXPECT_EQ(static_cast<std::size_t>(rd), written);
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
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  ConnectionState state;
  // Create an empty file to ensure sendfile has nothing to send
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, std::string());
  state.fileSend.filePayload = FilePayload(File(tmp.filePath().string()), 0, 0);
  state.fileSendActive = true;

  const auto res = state.transportFile(sv[0], /*tlsFlow=*/false);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::WouldBlock);
  EXPECT_TRUE(res.enableWritable);
}

TEST(ConnectionStateSendfileTest, TlsSendfileEmptyBufferClearsActive) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  ConnectionState state;
  // Empty file to ensure pread returns 0 and no tunnel buffer is filled
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, std::string());

  state.fileSend.filePayload = FilePayload(File(tmp.filePath().string()), 0, 0);

  state.fileSendActive = true;

  // Call transportFile in TLS mode which uses pread into tunnelOrFileBuffer
  const auto res = state.transportFile(sv[0], /*tlsFlow=*/true);

  EXPECT_EQ(res.bytesDone, 0U);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::Read);

  // After calling with empty file, tunnelOrFileBuffer should be empty and fileSendActive false
  EXPECT_EQ(state.tunnelOrFileBuffer.empty(), true);
  EXPECT_FALSE(state.fileSendActive);
}

TEST(ConnectionStateBufferTest, ShrinkToFitReducesNonEmptyBuffers) {
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  AsyncHandlerStatePool asyncStatePool;
#endif
  ConnectionState state;

  // Grow buffers to have extra capacity
  state.inBuffer.reserve(2048);
  state.inBuffer.append("hello world");

  state.bodyAndTrailersBuffer.reserve(1025);
  state.bodyAndTrailersBuffer.append("chunked body");

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  (void)state.ensureAsyncState(asyncStatePool);
  state.asyncState->headBuffer.reserve(4096);
  state.asyncState->headBuffer.append("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
#endif

  // Sanity: capacities should be larger than sizes prior to shrink
  EXPECT_GT(state.inBuffer.capacity(), state.inBuffer.size());
  EXPECT_GT(state.bodyAndTrailersBuffer.capacity(), state.bodyAndTrailersBuffer.size());
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  EXPECT_GT(state.asyncState->headBuffer.capacity(), state.asyncState->headBuffer.size());
#endif

  const auto oldCapacityInBuffer = state.inBuffer.capacity();
  const auto oldCapacityBodyBuffer = state.bodyAndTrailersBuffer.capacity();
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  const auto oldCapacityHeadBuffer = state.asyncState->headBuffer.capacity();
#endif

  state.reset();

  // After shrink and clear, capacities should be bounded by sizes
  EXPECT_LT(state.inBuffer.capacity(), oldCapacityInBuffer);
  EXPECT_LT(state.bodyAndTrailersBuffer.capacity(), oldCapacityBodyBuffer);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  EXPECT_FALSE(static_cast<bool>(state.asyncState));
  EXPECT_GT(oldCapacityHeadBuffer, 0U);
#endif
}

TEST(ConnectionStateBufferTest, ShrinkToFitOnEmptyBuffersYieldsZeroCapacity) {
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  AsyncHandlerStatePool asyncStatePool;
#endif
  ConnectionState state;

  // Ensure buffers are empty
  state.tunnelOrFileBuffer.clear();
  state.inBuffer.clear();
  state.bodyAndTrailersBuffer.clear();
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  (void)state.ensureAsyncState(asyncStatePool);
  state.asyncState->headBuffer.clear();
#endif

  state.reset();

  // Empty buffers should have capacity 0 after shrink_to_fit
  EXPECT_EQ(state.tunnelOrFileBuffer.capacity(), 0U);
  EXPECT_EQ(state.inBuffer.capacity(), 0U);
  EXPECT_EQ(state.bodyAndTrailersBuffer.capacity(), 0U);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  EXPECT_FALSE(static_cast<bool>(state.asyncState));
#endif
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

TEST(ConnectionStateBridgeTest, AggregatedBridgeReadOffsetPastEndHandled) {
  ConnectionState state;
  state.installAggregatedBodyBridge();

  // Provide an empty body so offset (0) is already past/equal to size (0).
  state.bodyStreamContext.body = std::string_view();
  state.bodyStreamContext.offset = 0;

  // readBody should see offset >= body.size() and return empty without crashing.
  auto chunk = state.request.readBody(4);
  EXPECT_TRUE(chunk.empty());
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

#ifdef AERONET_ENABLE_ASYNC_HANDLERS

TEST(ConnectionStateAsyncStateTest, ResetDestroysTheCoroutine) {
  AsyncHandlerStatePool asyncStatePool;
  ConnectionState state;
  (void)state.ensureAsyncState(asyncStatePool);

  // A coroutine created suspended, never started.
  auto makeSuspended = [] -> RequestTask<HttpResponse> { co_return HttpResponse(http::StatusCodeOK); };
  state.asyncState->task.coroutine = makeSuspended();
  EXPECT_TRUE(state.asyncState->task.coroutine.valid());

  // reset() should destroy the coroutine along with the async state
  state.reset();
  EXPECT_FALSE(static_cast<bool>(state.asyncState));
}
#endif

// A write never marks the TLS handshake as established, even when the transport reports it done: only its finalization
// by the server (negotiated parameters, ALPN protocol setup) does.
TEST(ConnectionStateTransportTest, TransportWriteBuffersDoesNotMarkTlsEstablished) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  ConnectionState state;
  // attach a plain transport that writes to sv[0]
  state.transport = Transport(sv[0]);

  // ensure tlsEstablished is initially false
  state.tlsEstablished = false;

  const std::string_view resp[]{"HTTP/1.1 204 No Content\r\n", "Content-Length: 0\r\n\r\n"};
  const auto res = state.transportWrite(resp);

  EXPECT_GE(res.bytesProcessed, 0U);
  EXPECT_FALSE(state.tlsEstablished);
}

namespace {
// Simple controllable transport for unit tests
class FakeTransport final : public TransportBackend<FakeTransport> {
 public:
  explicit FakeTransport(bool handshakeInitially) : _handshakeDone(handshakeInitially) {}

  static TransportResult read(char* buf, std::size_t len) {
    // Write a small marker and report bytes read
    const char* kMarker = "X";
    if (len > 0) {
      buf[0] = kMarker[0];
      return {1U, TransportHint::None};
    }
    return {0U, TransportHint::None};
  }

  TransportResult write(std::string_view data) {
    _lastWrite.assign(data.begin(), data.end());
    return {data.size(), TransportHint::None};
  }

  void setHandshakeDone(bool val) { _handshakeDone = val; }

  [[nodiscard]] bool handshakeDone() const noexcept { return _handshakeDone; }

  [[nodiscard]] std::string lastWrite() const { return _lastWrite; }

 private:
  bool _handshakeDone{true};
  std::string _lastWrite;
};

// Transport whose reads return up to 'available' bytes, as a socket with that many bytes in its receive queue.
class QueueTransport final : public TransportBackend<QueueTransport> {
 public:
  explicit QueueTransport(std::size_t* available) : _available(available) {}

  TransportResult read(char* buf, std::size_t len) {
    const std::size_t nbRead = std::min(len, *_available);
    std::memset(buf, 'q', nbRead);
    return {nbRead, nbRead == 0 ? TransportHint::ReadReady : TransportHint::None};
  }

  static TransportResult write(std::string_view data) { return {data.size(), TransportHint::None}; }

  [[nodiscard]] static bool handshakeDone() noexcept { return true; }

 private:
  std::size_t* _available;
};

}  // namespace

TEST(ZerocopyBuffersTest, ReleasesBuffersAsTheirSendsComplete) {
  ZerocopyBuffers buffers;
  ZeroCopyState sends;

  sends.seqHi = 1;
  buffers.retain(test::MakeMessageData(100, 'a'), sends.nextSeq());
  sends.seqHi = 3;
  buffers.retain(test::MakeMessageData(200, 'b'), sends.nextSeq());
  EXPECT_EQ(buffers.bytes(), 300U);
  EXPECT_EQ(buffers.size(), 2U);

  buffers.releaseCompleted(sends);
  EXPECT_EQ(buffers.size(), 2U);

  sends.complete(0, 0);
  buffers.releaseCompleted(sends);
  EXPECT_EQ(buffers.bytes(), 200U);
  EXPECT_EQ(buffers.size(), 1U);

  // Send 1 may still read the second buffer.
  sends.complete(2, 2);
  buffers.releaseCompleted(sends);
  EXPECT_EQ(buffers.bytes(), 200U);

  sends.complete(1, 1);
  buffers.releaseCompleted(sends);
  EXPECT_EQ(buffers.bytes(), 0U);
  EXPECT_TRUE(buffers.empty());
}

TEST(ZerocopyBuffersTest, RetainedBuffersCountForAtLeastTheMinimumSize) {
  static constexpr std::size_t kMinBytes = 1000;
  ZerocopyBuffers buffers;
  // Anything when nothing is retained: a single write may exceed the limit.
  EXPECT_TRUE(buffers.canRetain(64, 64UL * 100, kMinBytes, 4000));
  EXPECT_TRUE(buffers.canRetain(1, 1UL << 20U, kMinBytes, 4000));

  // A small buffer counts for kMinBytes.
  buffers.retain(test::MakeMessageData(100, 'a'), 1);
  EXPECT_TRUE(buffers.canRetain(1, 3000, kMinBytes, 4000));
  EXPECT_FALSE(buffers.canRetain(1, 3001, kMinBytes, 4000));
  // So do the small messages of a write.
  EXPECT_TRUE(buffers.canRetain(3, 300, kMinBytes, 4000));
  EXPECT_FALSE(buffers.canRetain(4, 400, kMinBytes, 4000));
  // Without minimum, only their sizes count.
  EXPECT_TRUE(buffers.canRetain(4, 400, 0, 4000));
  EXPECT_FALSE(buffers.canRetain(4, 3901, 0, 4000));

  // A larger buffer counts for its size: 1000 + 2500 retained.
  buffers.retain(test::MakeMessageData(2500, 'b'), 2);
  EXPECT_TRUE(buffers.canRetain(1, 1000, kMinBytes, 4500));
  EXPECT_FALSE(buffers.canRetain(1, 1001, kMinBytes, 4500));
}

TEST(ZerocopyBuffersTest, ExtractMovesOutAllBuffers) {
  ZerocopyBuffers buffers;
  buffers.retain(test::MakeMessageData(10, 'a'), 1);
  buffers.retain(test::MakeMessageData(20, 'b'), 2);

  std::string extracted;
  buffers.extract([&extracted](HttpMessageData data) { extracted.append(data.firstBuffer()); });
  EXPECT_EQ(extracted, std::string(10, 'a') + std::string(20, 'b'));
  EXPECT_TRUE(buffers.empty());
  EXPECT_EQ(buffers.bytes(), 0U);
}

#ifdef AERONET_LINUX
namespace {

// A plain connection over one end of an AF_UNIX socket pair, whose MSG_ZEROCOPY sends and their completion
// notifications are simulated by the sendmsg / recvmsg(MSG_ERRQUEUE) overrides.
class ZerocopyConnection {
 public:
  ZerocopyConnection() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
      _fds[0] = BaseFd(fds[0]);
      _fds[1] = BaseFd(fds[1]);
    }
    state.transport = Transport(_fds[0].fd());
    state.zerocopyEnabled = true;
  }

  ZerocopyConnection(const ZerocopyConnection&) = delete;
  ZerocopyConnection(ZerocopyConnection&&) noexcept = delete;
  ZerocopyConnection& operator=(const ZerocopyConnection&) = delete;
  ZerocopyConnection& operator=(ZerocopyConnection&&) noexcept = delete;

  ~ZerocopyConnection() {
    test::g_sendmsg_actions.reset();
    test::g_recvmsg_actions.reset();
    test::g_errqueue_notifications.reset();
  }

  // Simulated MSG_ZEROCOPY send of the first 'size' unsent bytes of 'data'.
  void sendZerocopy(const HttpMessageData& data, std::size_t size) {
    test::PushSendmsgAction(fd(), IoAction{static_cast<int64_t>(size), 0});
    const std::string_view buffers[]{data.firstBuffer(), data.secondBuffer()};
    const auto [bytesSent, status] = state.transport.get<PlainTransport>()->writeZerocopy(buffers);
    EXPECT_EQ(status, ZerocopySendStatus::Sent);
    EXPECT_EQ(bytesSent, size);
  }

  // Sends 'size' bytes of 'ch' with MSG_ZEROCOPY, then retains them, like ConnectionIo::queueData().
  void sendAndRetain(std::size_t size, char ch) {
    HttpMessageData data(RawChars(std::string(size, ch)));
    sendZerocopy(data, size);
    state.retainZerocopyBuffer(std::move(data));
  }

  // Delivers the completion notification of the sends 'first' to 'last', and polls it.
  ZerocopyCompletions complete(uint32_t first, uint32_t last, bool copied = false) {
    test::PushErrQueueNotification(
        fd(), {.first = first, .last = last, .code = static_cast<uint8_t>(copied ? SO_EE_CODE_ZEROCOPY_COPIED : 0)});
    // Ends the poll when sends remain in flight.
    test::g_recvmsg_actions.setActions(fd(), {IoAction{-1, error::kWouldBlock}});
    const auto completions = state.pollZerocopyCompletions();
    test::g_recvmsg_actions.reset();
    return completions;
  }

  [[nodiscard]] int fd() const noexcept { return _fds[0].fd(); }

  ConnectionState state;

 private:
  BaseFd _fds[2];
};

}  // namespace

TEST(ConnectionStateZerocopyTest, NoZerocopySendWithoutPlainTransport) {
  ConnectionState withoutTransport;
  EXPECT_FALSE(withoutTransport.hasZerocopySendsInFlight());
  EXPECT_EQ(withoutTransport.pollZerocopyCompletions().nbSends, 0U);

  ConnectionState custom;
  custom.transport = std::make_unique<FakeTransport>(/*handshakeInitially=*/true);
  EXPECT_FALSE(custom.hasZerocopySendsInFlight());
  EXPECT_EQ(custom.pollZerocopyCompletions().nbSends, 0U);
}

TEST(ConnectionStateZerocopyTest, CompletionsReleaseRetainedBuffersProgressively) {
  ZerocopyConnection cnx;
  cnx.sendAndRetain(600, 'a');
  cnx.sendAndRetain(400, 'b');
  EXPECT_TRUE(cnx.state.hasZerocopySendsInFlight());
  EXPECT_EQ(cnx.state.zerocopyBuffers.bytes(), 1000U);

  // Each buffer is released as soon as the sends that may read it completed, not once all of them did.
  EXPECT_EQ(cnx.complete(0, 0).nbSends, 1U);
  EXPECT_TRUE(cnx.state.hasZerocopySendsInFlight());
  EXPECT_EQ(cnx.state.zerocopyBuffers.bytes(), 400U);

  EXPECT_EQ(cnx.complete(1, 1).nbSends, 1U);
  EXPECT_FALSE(cnx.state.hasZerocopySendsInFlight());
  EXPECT_TRUE(cnx.state.zerocopyBuffers.empty());
  EXPECT_EQ(cnx.state.zerocopyBuffers.bytes(), 0U);
}

TEST(ConnectionStateZerocopyTest, OutOfOrderCompletionKeepsTheBuffersOfEarlierSends) {
  ZerocopyConnection cnx;
  cnx.sendAndRetain(600, 'a');
  cnx.sendAndRetain(400, 'b');

  // The kernel may still read the first buffer (retransmission of send 0).
  cnx.complete(1, 1);
  EXPECT_TRUE(cnx.state.hasZerocopySendsInFlight());
  EXPECT_EQ(cnx.state.zerocopyBuffers.bytes(), 1000U);

  cnx.complete(0, 0);
  EXPECT_FALSE(cnx.state.hasZerocopySendsInFlight());
  EXPECT_TRUE(cnx.state.zerocopyBuffers.empty());
}

TEST(ConnectionStateZerocopyTest, CopiedSendStopsZerocopyWhenRequested) {
  for (const bool stopOnCopy : {false, true}) {
    ZerocopyConnection cnx;
    cnx.state.zerocopyStopOnCopy = stopOnCopy;
    cnx.sendAndRetain(100, 'a');
    const auto completions = cnx.complete(0, 0, /*copied=*/true);
    EXPECT_EQ(completions.nbCopiedSends, 1U);
    EXPECT_EQ(cnx.state.zerocopyEnabled, !stopOnCopy);
  }
}

namespace {

// Queues 'size' bytes of 'ch', then writes the first 'written' of them with MSG_ZEROCOPY.
void QueueAndSendZerocopy(ZerocopyConnection& cnx, std::size_t size, char ch, std::size_t written) {
  cnx.state.queueOutput(test::MakeMessageData(size, ch));
  cnx.sendZerocopy(cnx.state.outQueue.front(), written);
  cnx.state.outputWritten(written, /*zerocopySent=*/true);
}

}  // namespace

TEST(ConnectionStateZerocopyTest, ZerocopySentFirstEntryIsNeitherShrunkNorModifiedInPlace) {
  ZerocopyConnection cnx;
  ConnectionState& state = cnx.state;
  // Partially written with MSG_ZEROCOPY: the kernel reads its first 300 bytes until the send completes.
  QueueAndSendZerocopy(cnx, 1000, 'o', 300);
  EXPECT_TRUE(state.outQueueFrontZerocopySent);
  const char* storage = state.outQueue.front().firstBuffer().data();

  state.reclaimMemoryFromOversizedBuffers();
  EXPECT_EQ(state.outQueue.front().firstBuffer().data(), storage);

  // Output produced meanwhile, even small, is queued behind it: neither of them is copied.
  EXPECT_FALSE(state.queueOutput(test::MakeMessageData(16, 'n')));
  EXPECT_TRUE(state.outQueueFrontZerocopySent);
  ASSERT_EQ(state.outQueue.size(), 2U);
  EXPECT_EQ(state.outQueue.front().firstBuffer().data(), storage);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(700, 'o'));
  EXPECT_EQ(state.outQueue.entries()[1].firstBuffer(), std::string(16, 'n'));
  EXPECT_EQ(state.bufferedOutputBytes(), 716U);
  EXPECT_TRUE(state.zerocopyBuffers.empty());

  // Not written yet, the entry behind it is appended to.
  EXPECT_TRUE(state.queueOutput(test::MakeMessageData(4, 'm')));
  ASSERT_EQ(state.outQueue.size(), 2U);
  EXPECT_EQ(state.outQueue.entries()[1].firstBuffer(), std::string(16, 'n') + std::string(4, 'm'));

  cnx.complete(0, 0);
  EXPECT_FALSE(state.outQueueFrontZerocopySent);
}

TEST(ConnectionStateZerocopyTest, FirstEntryIsAppendedToOnceItsSendsCompleted) {
  ZerocopyConnection cnx;
  ConnectionState& state = cnx.state;
  QueueAndSendZerocopy(cnx, 1000, 'o', 300);

  cnx.complete(0, 0);
  EXPECT_FALSE(state.outQueueFrontZerocopySent);

  EXPECT_TRUE(state.queueOutput(test::MakeMessageData(16, 'n')));
  ASSERT_EQ(state.outQueue.size(), 1U);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(700, 'o') + std::string(16, 'n'));
  EXPECT_TRUE(state.zerocopyBuffers.empty());
}

TEST(ConnectionStateZerocopyTest, GatheredZerocopyWriteRetainsTheWrittenEntries) {
  ZerocopyConnection cnx;
  ConnectionState& state = cnx.state;
  static constexpr std::size_t kEntrySize = OutputQueue::kMaxMergedSize;
  state.queueOutput(test::MakeMessageData(kEntrySize, 'a'));
  state.queueOutput(test::MakeMessageData(kEntrySize, 'b'));
  state.queueOutput(test::MakeMessageData(kEntrySize, 'c'));
  ASSERT_EQ(state.outQueue.size(), 3U);
  const char* lastStorage = state.outQueue.entries()[2].firstBuffer().data();

  // One send of the first 2 entries, and of the start of the third one.
  test::PushSendmsgAction(cnx.fd(), IoAction{static_cast<int64_t>((2 * kEntrySize) + 100), 0});
  vector<std::string_view> buffers;
  const auto gathered = state.gatherOutput(buffers, ZeroCopyState::kMaxBuffers);
  EXPECT_EQ(gathered.size, 3 * kEntrySize);
  EXPECT_EQ(gathered.nbMessages, 3U);
  const auto [bytesSent, status] = state.transport.get<PlainTransport>()->writeZerocopy(buffers);
  ASSERT_EQ(status, ZerocopySendStatus::Sent);
  state.outputWritten(bytesSent, /*zerocopySent=*/true);

  // The kernel reads the 2 fully written buffers, and the one written in part, still queued, until the completion.
  EXPECT_EQ(state.zerocopyBuffers.size(), 2U);
  EXPECT_EQ(state.zerocopyBuffers.bytes(), 2 * kEntrySize);
  ASSERT_EQ(state.outQueue.size(), 1U);
  EXPECT_TRUE(state.outQueueFrontZerocopySent);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(kEntrySize - 100, 'c'));
  EXPECT_EQ(state.outQueue.front().firstBuffer().data(), lastStorage + 100);
  EXPECT_EQ(state.bufferedOutputBytes(), kEntrySize - 100);

  cnx.complete(0, 0);
  EXPECT_TRUE(state.zerocopyBuffers.empty());
  EXPECT_FALSE(state.outQueueFrontZerocopySent);
}

TEST(ConnectionStateZerocopyTest, EntriesWrittenWithoutZerocopyAfterAZerocopySentOneAreReleased) {
  ZerocopyConnection cnx;
  ConnectionState& state = cnx.state;
  static constexpr std::size_t kEntrySize = OutputQueue::kMaxMergedSize;
  QueueAndSendZerocopy(cnx, kEntrySize, 'a', 100);
  state.queueOutput(test::MakeMessageData(kEntrySize, 'b'));
  state.queueOutput(test::MakeMessageData(kEntrySize, 'c'));

  // The rest of the first entry, the second one and the start of the third one, copied.
  state.outputWritten((2 * kEntrySize) - 100 + 10, /*zerocopySent=*/false);
  // Only the first one is read by the kernel.
  EXPECT_EQ(state.zerocopyBuffers.size(), 1U);
  EXPECT_EQ(state.zerocopyBuffers.bytes(), kEntrySize);
  ASSERT_EQ(state.outQueue.size(), 1U);
  EXPECT_FALSE(state.outQueueFrontZerocopySent);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(kEntrySize - 10, 'c'));
}

TEST(ConnectionStateZerocopyTest, ClearedFirstEntryIsRetainedWhileItsSendsAreInFlight) {
  ZerocopyConnection cnx;
  ConnectionState& state = cnx.state;
  QueueAndSendZerocopy(cnx, 1000, 'o', 300);
  state.queueOutput(test::MakeMessageData(OutputQueue::kMaxMergedSize, 'n'));

  // Dropped (failed write, epoll failure): the kernel still reads the first entry.
  state.clearBuffers();
  EXPECT_TRUE(state.outQueue.empty());
  EXPECT_FALSE(state.outQueueFrontZerocopySent);
  EXPECT_EQ(state.zerocopyBuffers.size(), 1U);
  EXPECT_EQ(state.zerocopyBuffers.bytes(), 1000U);
  EXPECT_TRUE(state.isDrainCloseRequested());
  // A drain close waits for the completion of the zerocopy sends: after close, the kernel would still send from their
  // buffers, released with the connection state.
  EXPECT_FALSE(state.canCloseConnectionForDrain());

  cnx.complete(0, 0);
  EXPECT_TRUE(state.zerocopyBuffers.empty());
  EXPECT_TRUE(state.canCloseConnectionForDrain());
}

TEST(ConnectionStateZerocopyTest, ClearedOutputWithoutZerocopySendIsReleased) {
  ZerocopyConnection cnx;
  ConnectionState& state = cnx.state;
  state.queueOutput(test::MakeMessageData(1000, 'o'));
  state.outputWritten(300, /*zerocopySent=*/false);
  state.clearBuffers();
  EXPECT_TRUE(state.outQueue.empty());
  EXPECT_TRUE(state.zerocopyBuffers.empty());
}

TEST(ConnectionStateZerocopyTest, ResetForgetsTheZerocopyState) {
  ZerocopyConnection cnx;
  ConnectionState& state = cnx.state;
  state.zerocopyStopOnCopy = true;
  state.queueOutput(test::MakeMessageData(10, 'b'));
  state.outQueueFrontZerocopySent = true;
  state.zerocopyStallCheckQueuedBytes = 42;
  state.zerocopyBuffers.retain(test::MakeMessageData(10, 'a'), 1);

  state.reset();
  EXPECT_FALSE(state.zerocopyEnabled);
  EXPECT_FALSE(state.zerocopyStopOnCopy);
  EXPECT_TRUE(state.outQueue.empty());
  EXPECT_FALSE(state.outQueueFrontZerocopySent);
  EXPECT_EQ(state.zerocopyStallCheckQueuedBytes, ConnectionState::kNoZerocopyStallCheck);
  EXPECT_TRUE(state.zerocopyBuffers.empty());
}

TEST(ConnectionStateZerocopyTest, PolicyFollowsModeAndPeerAddress) {
  const auto ipv4 = [](const char* address) {
    sockaddr_storage storage{};
    auto* addr = reinterpret_cast<sockaddr_in*>(&storage);
    addr->sin_family = AF_INET;
    EXPECT_EQ(::inet_pton(AF_INET, address, &addr->sin_addr), 1);
    return storage;
  };
  sockaddr_storage unixPeer{};
  unixPeer.ss_family = AF_UNIX;

  struct Case {
    ZerocopyMode mode;
    sockaddr_storage peer;
    bool enabled;
    bool stopOnCopy;
  };
  for (const Case& testCase : {
           Case{ZerocopyMode::Disabled, ipv4("10.1.2.3"), false, false},
           Case{ZerocopyMode::Enabled, ipv4("127.0.0.1"), true, false},
           Case{ZerocopyMode::Opportunistic, ipv4("127.0.0.1"), false, true},
           Case{ZerocopyMode::Opportunistic, ipv4("10.1.2.3"), true, true},
           Case{ZerocopyMode::Opportunistic, unixPeer, false, true},
       }) {
    HttpServerConfig config;
    config.withZerocopyMode(testCase.mode);
    CompressionState compressionState(config.compression);
    ConnectionState state;
    state.initializeStateNewConnection(config, testCase.peer, compressionState);
    EXPECT_EQ(state.zerocopyEnabled, testCase.enabled) << static_cast<int>(testCase.mode);
    EXPECT_EQ(state.zerocopyStopOnCopy, testCase.stopOnCopy) << static_cast<int>(testCase.mode);
  }
}
#endif

TEST(ConnectionStateTransportTest, TransportWriteStringDoesNotMarkTlsEstablished) {
  ConnectionState state;
  state.transport = std::make_unique<FakeTransport>(/*handshakeInitially=*/true);
  state.tlsEstablished = false;

  const auto res = state.transportWrite(std::string_view{"hello"});
  EXPECT_EQ(res.want, TransportHint::None);
  EXPECT_EQ(res.bytesProcessed, 5U);
  EXPECT_FALSE(state.tlsEstablished);
}

TEST(ConnectionStateTransportTest, TransportWriteStringCompletingHandshakeDoesNotMarkTlsEstablished) {
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

  // The handshake is now done: still not marked established, as it was not finalized.
  raw->setHandshakeDone(true);
  const auto res2 = state.transportWrite(std::string_view{"def"});
  EXPECT_EQ(res2.want, TransportHint::None);
  EXPECT_EQ(res2.bytesProcessed, 3U);
  EXPECT_FALSE(state.tlsEstablished);
}

TEST(ConnectionStateTransportTest, ReadSizeAdaptsToTheBytesRead) {
  HttpServerConfig config;
  config.withMinReadChunkBytes(4096).withMaxPerEventReadBytes(std::numeric_limits<std::uint32_t>::max());
  std::size_t available = 1UL << 30U;
  ConnectionState state;
  state.transport = std::make_unique<QueueTransport>(&available);

  const auto read = [&config, &state] {
    const std::size_t chunkSize = config.computeReadChunkSize(0, state.readSizeShift);
    EXPECT_NE(state.transportRead(config, chunkSize).want, TransportHint::Error);
    state.inBuffer.clear();  // consumed
    return chunkSize;
  };

  // Reads filling the read size double it, up to kMaxReadChunkBytes.
  for (std::size_t expected = 4096; expected < HttpServerConfig::kMaxReadChunkBytes; expected *= 2U) {
    EXPECT_EQ(read(), expected);
  }
  EXPECT_EQ(read(), HttpServerConfig::kMaxReadChunkBytes);
  EXPECT_EQ(read(), HttpServerConfig::kMaxReadChunkBytes);
  EXPECT_EQ(state.readSizeShift, 6U);

  // Reads filling at least a quarter of it keep it.
  available = HttpServerConfig::kMaxReadChunkBytes / 4U;
  EXPECT_EQ(read(), HttpServerConfig::kMaxReadChunkBytes);
  EXPECT_EQ(read(), HttpServerConfig::kMaxReadChunkBytes);

  // A read returning nothing (would block) keeps it.
  available = 0;
  EXPECT_EQ(read(), HttpServerConfig::kMaxReadChunkBytes);

  // Smaller reads halve it, down to minReadChunkBytes.
  available = 100;
  for (std::size_t expected = HttpServerConfig::kMaxReadChunkBytes; expected > 4096; expected /= 2U) {
    EXPECT_EQ(read(), expected);
  }
  EXPECT_EQ(read(), 4096U);
  EXPECT_EQ(read(), 4096U);
  EXPECT_EQ(state.readSizeShift, 0U);

  // A connection state reused for a new connection starts again from minReadChunkBytes.
  available = 1UL << 30U;
  EXPECT_EQ(read(), 4096U);
  EXPECT_EQ(read(), 8192U);
  state.reset();
  EXPECT_EQ(state.readSizeShift, 0U);
}

TEST(ConnectionStateTransportTest, ReadsCutByTheBudgetKeepTheReadSize) {
  HttpServerConfig config;
  config.withMinReadChunkBytes(4096).withMaxPerEventReadBytes(48UL << 10U);
  std::size_t available = 1UL << 30U;
  ConnectionState state;
  state.transport = std::make_unique<QueueTransport>(&available);
  state.readSizeShift = 3;  // 32 KiB

  std::size_t bytesReadThisEvent = 0;
  std::size_t chunkSize = config.computeReadChunkSize(bytesReadThisEvent, state.readSizeShift);
  EXPECT_EQ(chunkSize, 32UL << 10U);
  bytesReadThisEvent += state.transportRead(config, chunkSize).bytesProcessed;
  EXPECT_EQ(state.readSizeShift, 4U);

  // The rest of the budget: 16 KiB of the 64 KiB read size.
  chunkSize = config.computeReadChunkSize(bytesReadThisEvent, state.readSizeShift);
  EXPECT_EQ(chunkSize, 16UL << 10U);
  bytesReadThisEvent += state.transportRead(config, chunkSize).bytesProcessed;
  EXPECT_EQ(bytesReadThisEvent, 48UL << 10U);
  EXPECT_EQ(state.readSizeShift, 4U);
  EXPECT_EQ(state.inBuffer.size(), 48UL << 10U);
}

TEST(ConnectionStateTransportTest, TransportReadDoesNotStartHeaderReadTimer) {
  // The header read timer is the one of the HTTP/1.1 read loop of the server: the tunnel relays read with transportRead
  // too.
  ConnectionState state;
  state.transport = std::make_unique<FakeTransport>(/*handshakeInitially=*/true);
  state.lastActivity = std::chrono::steady_clock::now();
  const HttpServerConfig config;

  const auto result = state.transportRead(config, 16U);
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_EQ(result.bytesProcessed, 1U);
  EXPECT_EQ(state.headerStartTp.time_since_epoch().count(), 0);
  EXPECT_FALSE(state.parsingHeaders);
}

TEST(ConnectionStateTransportTest, TransportWriteBuffersCompletingHandshakeDoesNotMarkTlsEstablished) {
  ConnectionState state;
  auto fake = std::make_unique<FakeTransport>(/*handshakeInitially=*/false);
  FakeTransport* raw = fake.get();
  state.transport = std::move(fake);
  state.tlsEstablished = false;

  const std::string_view resp[]{"HTTP/1.1 204 No Content\r\n", "Content-Length: 0\r\n\r\n"};

  // First write: handshake not done yet, tlsEstablished should remain false
  const auto res1 = state.transportWrite(resp);
  EXPECT_EQ(res1.want, TransportHint::None);
  EXPECT_GE(res1.bytesProcessed, 0U);
  EXPECT_FALSE(state.tlsEstablished);

  // The handshake is now done: still not marked established, as it was not finalized.
  raw->setHandshakeDone(true);
  const auto res2 = state.transportWrite(resp);
  EXPECT_EQ(res2.want, TransportHint::None);
  EXPECT_GE(res2.bytesProcessed, 0U);
  EXPECT_FALSE(state.tlsEstablished);
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

TEST(ConnectionStateTransportTest, TransportWriteBuffersSkipsHandshakeWhenAlreadyEstablished) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  ConnectionState state;
  state.transport = Transport(sv[0]);
  state.tlsEstablished = true;  // simulate prior completion

  const std::string_view resp[]{"HTTP/1.1 200 OK\r\n", "Content-Length: 0\r\n\r\n"};
  const auto res = state.transportWrite(resp);
  EXPECT_GE(res.bytesProcessed, 0U);
  EXPECT_TRUE(state.tlsEstablished);  // remains true; handshake not re-checked
}

TEST(ConnectionStateSendfileTest, TlsPreadErrorTriggersCloseAndClearsActive) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[]{BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(16, 'E');
  test::ScopedTempDir tmpDir;
  ScopedTempFile tmp(tmpDir, content);

  ConnectionState state;
  state.fileSend.filePayload = FilePayload(File(tmp.filePath().string()), 0, content.size());
  state.fileSendActive = true;
  // Output queued behind the file: it must not follow its truncated body.
  EXPECT_FALSE(state.queueOutput(test::MakeMessageData(16, 'q')));

  // Force a hard pread error (not EAGAIN/error::kInterrupted) to take default error path
  test::SetPreadPathActions(tmp.filePath().string(), {IoAction{-1, EIO}});

  auto res = state.transportFile(sv[0], /*tlsFlow=*/true);
  EXPECT_EQ(res.code, ConnectionState::FileResult::Code::Error);
  EXPECT_EQ(res.bytesDone, 0U);
  EXPECT_FALSE(state.fileSendActive);
  EXPECT_FALSE(state.hasPendingOutput());
  EXPECT_TRUE(state.isAnyCloseRequested());
}

// A connection closed in the middle of a file transfer is cached, then reset for a new connection, which must not
// inherit the transfer: its requests would never be processed while a file send looks in progress.
TEST(ConnectionStateTest, ResetEndsAnInterruptedFileTransfer) {
  ConnectionState state;
  EXPECT_TRUE(state.attachFilePayload(FilePayload{File{}, 0, 4}));
  state.queueOutput(HttpMessageData{RawChars("HTTP/1.1 200 OK\r\n\r\n")});  // the next response, sent after it
  ASSERT_TRUE(state.isSendingFile());

  state.reset();

  EXPECT_FALSE(state.isSendingFile());
  EXPECT_FALSE(state.hasPendingOutput());
  EXPECT_EQ(state.fileSend.filePayload.length, 0U);
}

TEST(ConnectionStateTest, AttachFilePayloadReturnsWhetherThereIsSomethingToSend) {
  ConnectionState state;
  EXPECT_FALSE(state.attachFilePayload(FilePayload{File{}, 2, 0}));
  EXPECT_FALSE(state.isSendingFile());

  EXPECT_TRUE(state.attachFilePayload(FilePayload{File{}, 2, 4}));
  EXPECT_TRUE(state.isSendingFile());
}

namespace {

constexpr std::string_view kFileResponseHead = "HTTP/1.1 200 OK\r\ncontent-length: 4\r\n\r\n";

}  // namespace

namespace {

HttpMessageData FileResponse(std::size_t offset = 0) {
  return {RawChars(kFileResponseHead), HttpPayload(FilePayload{File{}, offset, 4})};
}

}  // namespace

TEST(ConnectionStateOutputTest, SmallOutputIsAppendedToTheLastEntry) {
  ConnectionState state;
  EXPECT_FALSE(state.queueOutput(test::MakeMessageData(100, 'a')));
  EXPECT_TRUE(state.queueOutput(test::MakeMessageData(50, 'b')));
  ASSERT_EQ(state.outQueue.size(), 1U);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(100, 'a') + std::string(50, 'b'));
  EXPECT_EQ(state.bufferedOutputBytes(), 150U);
  EXPECT_TRUE(state.hasPendingOutput());

  // Even written in part.
  state.outputWritten(30, /*zerocopySent=*/false);
  EXPECT_EQ(state.bufferedOutputBytes(), 120U);
  EXPECT_TRUE(state.queueOutput(test::MakeMessageData(10, 'c')));
  ASSERT_EQ(state.outQueue.size(), 1U);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(70, 'a') + std::string(50, 'b') + std::string(10, 'c'));
  EXPECT_EQ(state.bufferedOutputBytes(), 130U);
}

TEST(ConnectionStateOutputTest, LargeOutputIsQueuedWithoutCopy) {
  ConnectionState state;
  state.queueOutput(test::MakeMessageData(100, 'a'));
  HttpMessageData large = test::MakeMessageData(OutputQueue::kMaxMergedSize, 'b');
  const char* storage = large.firstBuffer().data();
  EXPECT_FALSE(state.queueOutput(std::move(large)));
  // Not merged into the large entry either.
  EXPECT_FALSE(state.queueOutput(test::MakeMessageData(10, 'c')));

  ASSERT_EQ(state.outQueue.size(), 3U);
  EXPECT_EQ(state.outQueue.entries()[0].firstBuffer(), std::string(100, 'a'));
  EXPECT_EQ(state.outQueue.entries()[1].firstBuffer().data(), storage);
  EXPECT_EQ(state.outQueue.entries()[2].firstBuffer(), std::string(10, 'c'));
  EXPECT_EQ(state.bufferedOutputBytes(), 110U + OutputQueue::kMaxMergedSize);
}

TEST(ConnectionStateOutputTest, OutputBehindAFileIsQueued) {
  ConnectionState state;
  EXPECT_TRUE(state.attachFilePayload(FilePayload{File{}, 0, 4}));

  // Sent after the file, merged together.
  EXPECT_FALSE(state.queueOutput(test::MakeMessageData(10, 'b')));
  EXPECT_TRUE(state.queueOutput(test::MakeMessageData(10, 'c')));
  ASSERT_EQ(state.outQueue.size(), 1U);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(10, 'b') + std::string(10, 'c'));

  // A file response is never merged, nor merged into.
  EXPECT_FALSE(state.queueOutput(FileResponse()));
  EXPECT_FALSE(state.queueOutput(test::MakeMessageData(10, 'd')));
  EXPECT_EQ(state.outQueue.size(), 3U);
}

TEST(ConnectionStateOutputTest, QueuedFileIsAttachedOnceItsResponseHeadIsWritten) {
  ConnectionState state;
  state.queueOutput(test::MakeMessageData(10, 'a'));
  state.queueOutput(FileResponse(3));
  state.queueOutput(test::MakeMessageData(10, 'b'));
  ASSERT_EQ(state.outQueue.size(), 3U);
  EXPECT_FALSE(state.isSendingFile());
  // The file is not counted.
  EXPECT_EQ(state.bufferedOutputBytes(), 20U + kFileResponseHead.size());

  // Up to the middle of its head.
  state.outputWritten(15, /*zerocopySent=*/false);
  EXPECT_FALSE(state.isSendingFile());
  EXPECT_EQ(state.outQueue.front().firstBuffer(), kFileResponseHead.substr(5));

  state.outputWritten(kFileResponseHead.size() - 5, /*zerocopySent=*/false);
  EXPECT_TRUE(state.isSendingFile());
  EXPECT_EQ(state.fileSend.filePayload.offset, 3U);
  EXPECT_EQ(state.fileSend.filePayload.length, 4U);
  ASSERT_EQ(state.outQueue.size(), 1U);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(10, 'b'));
  EXPECT_EQ(state.bufferedOutputBytes(), 10U);
}

TEST(ConnectionStateOutputTest, WrittenOutputIsReleasedInOrder) {
  static constexpr std::size_t kSize = OutputQueue::kMaxMergedSize;
  ConnectionState state;
  state.queueOutput(test::MakeMessageData(kSize, 'a'));
  state.queueOutput(test::MakeMessageData(kSize, 'b'));
  state.queueOutput(test::MakeMessageData(kSize, 'c'));

  state.outputWritten(100, /*zerocopySent=*/false);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(kSize - 100, 'a'));
  EXPECT_EQ(state.outQueue.size(), 3U);

  // The rest of the first entry and the whole second one, up to the third one.
  state.outputWritten((2 * kSize) - 100, /*zerocopySent=*/false);
  EXPECT_FALSE(state.outQueueFrontZerocopySent);
  ASSERT_EQ(state.outQueue.size(), 1U);
  EXPECT_FALSE(state.isSendingFile());
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(kSize, 'c'));
  EXPECT_EQ(state.bufferedOutputBytes(), kSize);

  // Written in part with the entry that follows it.
  state.queueOutput(test::MakeMessageData(kSize, 'd'));
  state.outputWritten(kSize + 10, /*zerocopySent=*/false);
  ASSERT_EQ(state.outQueue.size(), 1U);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(kSize - 10, 'd'));
  EXPECT_EQ(state.bufferedOutputBytes(), kSize - 10);
}

TEST(ConnectionStateOutputTest, GatheredOutputStopsAtTheHeadOfTheFirstFileResponse) {
  static constexpr std::size_t kSize = OutputQueue::kMaxMergedSize;
  ConnectionState state;
  state.queueOutput(test::MakeMessageData(kSize, 'a'));
  state.outputWritten(100, /*zerocopySent=*/false);
  state.queueOutput(test::MakeMessageData(kSize, 'b'));
  // A message with a payload: its head, then its body.
  state.queueOutput(HttpMessageData(RawChars("head"), HttpPayload(std::string(kSize, 'c'))));
  state.queueOutput(FileResponse());
  state.queueOutput(test::MakeMessageData(kSize, 'd'));

  vector<std::string_view> buffers{"stale"};
  const auto [size, retainedSize, nbMessages] = state.gatherOutput(buffers, ZeroCopyState::kMaxBuffers);
  ASSERT_EQ(buffers.size(), 5U);
  EXPECT_EQ(buffers[0], std::string(kSize - 100, 'a'));
  EXPECT_EQ(buffers[1], std::string(kSize, 'b'));
  EXPECT_EQ(buffers[2], "head");
  EXPECT_EQ(buffers[3], std::string(kSize, 'c'));
  EXPECT_EQ(buffers[4], kFileResponseHead);
  EXPECT_EQ(size, (3 * kSize) - 100 + 4 + kFileResponseHead.size());
  // The first entry retains its written start as well.
  EXPECT_EQ(retainedSize, (3 * kSize) + 4 + kFileResponseHead.size());
  EXPECT_EQ(nbMessages, 4U);

  // All written: the file is sent before the rest.
  state.outputWritten(size, /*zerocopySent=*/false);
  EXPECT_TRUE(state.isSendingFile());
  ASSERT_EQ(state.outQueue.size(), 1U);
  EXPECT_EQ(state.outQueue.front().firstBuffer(), std::string(kSize, 'd'));
}

TEST(ConnectionStateOutputTest, GatheredOutputIsBoundedByMaxBuffers) {
  static constexpr std::size_t kSize = OutputQueue::kMaxMergedSize;
  ConnectionState state;
  state.queueOutput(HttpMessageData(RawChars("head"), HttpPayload(std::string(kSize, 'a'))));
  // Its head written: only its body is left.
  state.outputWritten(4, /*zerocopySent=*/false);
  for (int pos = 0; pos < 10; ++pos) {
    state.queueOutput(test::MakeMessageData(kSize, static_cast<char>('b' + pos)));
  }

  vector<std::string_view> buffers;
  // Room for 2 buffers per message: 1 for the first entry, then 3 entries.
  const auto [size, retainedSize, nbMessages] = state.gatherOutput(buffers, 5);
  ASSERT_EQ(buffers.size(), 4U);
  EXPECT_EQ(buffers[0], std::string(kSize, 'a'));
  EXPECT_EQ(buffers[3], std::string(kSize, 'd'));
  EXPECT_EQ(size, 4 * kSize);
  EXPECT_EQ(retainedSize, (4 * kSize) + 4);
  EXPECT_EQ(nbMessages, 4U);
}

TEST(ConnectionStateOutputTest, ClearBuffersDropsTheQueueAndTheFile) {
  ConnectionState state;
  EXPECT_TRUE(state.attachFilePayload(FilePayload{File{}, 0, 4}));
  state.queueOutput(FileResponse());
  state.queueOutput(test::MakeMessageData(10, 'b'));

  state.clearBuffers();
  EXPECT_FALSE(state.hasPendingOutput());
  EXPECT_FALSE(state.isSendingFile());
  EXPECT_EQ(state.bufferedOutputBytes(), 0U);
  EXPECT_TRUE(state.canCloseConnectionForDrain());
}

TEST(ConnectionStateOutputTest, QueueIsKeptByReclaimAndDroppedByReset) {
  ConnectionState state;
  state.queueOutput(test::MakeMessageData(10, 'a'));
  state.queueOutput(test::MakeMessageData(OutputQueue::kMaxMergedSize, 'b'));

  state.reclaimMemoryFromOversizedBuffers();
  EXPECT_EQ(state.outQueue.size(), 2U);

  state.reset();
  EXPECT_TRUE(state.outQueue.empty());
  EXPECT_FALSE(state.hasPendingOutput());
}

namespace {
// Protocol handler with a given number of output bytes pending.
class PendingOutputProtocolHandler final : public IProtocolHandler {
 public:
  explicit PendingOutputProtocolHandler(std::size_t pendingBytes) : _pendingBytes(pendingBytes) {}

  [[nodiscard]] ProtocolType type() const noexcept override { return ProtocolType::WebSocket; }

  [[nodiscard]] ProtocolProcessResult processInput(std::span<const std::byte> /*data*/) override { return {}; }

  [[nodiscard]] std::span<const std::byte> getPendingOutput() const noexcept override { return {}; }

  [[nodiscard]] std::size_t pendingOutputSize() const noexcept override { return _pendingBytes; }

  void onOutputWritten(std::size_t /*bytesWritten*/) override {}

  void onTransportClosing() override {}

 private:
  std::size_t _pendingBytes;
};
}  // namespace

TEST(ConnectionStateCorkTest, NeverCorksWhenNotCorkable) {
  ConnectionState state;
  EXPECT_TRUE(state.attachFilePayload(FilePayload{File{}, 0, 4}));
  state.queueOutput(test::MakeMessageData(100, 'a'));
  state.queueOutput(FileResponse(3));

  EXPECT_FALSE(state.needsCork(1UL << 20U, true));
  EXPECT_FALSE(state.flushNeedsCork());
}

TEST(ConnectionStateCorkTest, SingleWritesAreNotCorked) {
  ConnectionState state;
  state.corkable = true;
  EXPECT_FALSE(state.encryptsOutputInUserSpace());

  // Without user-space TLS, the output is written in a single gather write, whatever its size.
  EXPECT_FALSE(state.needsCork(100, false));
  EXPECT_FALSE(state.needsCork(1UL << 20U, false));
  EXPECT_FALSE(state.flushNeedsCork());

  state.queueOutput(test::MakeMessageData(1UL << 16U, 'a'));
  EXPECT_FALSE(state.flushNeedsCork());
  // Gathered with it.
  state.queueOutput(test::MakeMessageData(OutputQueue::kMaxMergedSize, 'b'));
  ASSERT_EQ(state.outQueue.size(), 2U);
  EXPECT_FALSE(state.flushNeedsCork());

  // A file alone.
  ConnectionState fileState;
  fileState.corkable = true;
  EXPECT_TRUE(fileState.attachFilePayload(FilePayload{File{}, 0, 1UL << 20U}));
  EXPECT_FALSE(fileState.flushNeedsCork());

  // The protocol handler output alone.
  ConnectionState handlerState;
  handlerState.corkable = true;
  handlerState.protocolHandler = std::make_unique<PendingOutputProtocolHandler>(1UL << 20U);
  EXPECT_FALSE(handlerState.flushNeedsCork());
}

TEST(ConnectionStateCorkTest, OutputWrittenInSeveralPartsIsCorked) {
  ConnectionState state;
  state.corkable = true;
  EXPECT_TRUE(state.needsCork(10, true));

  // A response head, then its file.
  ConnectionState headThenFile;
  headThenFile.corkable = true;
  headThenFile.queueOutput(FileResponse());
  EXPECT_TRUE(headThenFile.flushNeedsCork());

  // A file, then the queued output.
  ConnectionState fileThenQueue;
  fileThenQueue.corkable = true;
  EXPECT_TRUE(fileThenQueue.attachFilePayload(FilePayload{File{}, 0, 4}));
  fileThenQueue.queueOutput(test::MakeMessageData(10, 'b'));
  EXPECT_TRUE(fileThenQueue.flushNeedsCork());

  // Queued output with a file response: gathered up to it.
  ConnectionState queuedFile;
  queuedFile.corkable = true;
  queuedFile.queueOutput(test::MakeMessageData(10, 'a'));
  queuedFile.queueOutput(FileResponse(3));
  EXPECT_TRUE(queuedFile.flushNeedsCork());

  // The HTTP/1 output (a 101 Switching Protocols response), then the protocol handler output.
  ConnectionState upgraded;
  upgraded.corkable = true;
  upgraded.queueOutput(test::MakeMessageData(10, 'a'));
  upgraded.protocolHandler = std::make_unique<PendingOutputProtocolHandler>(10);
  EXPECT_TRUE(upgraded.flushNeedsCork());
  // Unless the protocol handler has nothing to write.
  upgraded.protocolHandler = std::make_unique<PendingOutputProtocolHandler>(0);
  EXPECT_FALSE(upgraded.flushNeedsCork());
}

#ifdef AERONET_ENABLE_OPENSSL
namespace {
// TLS without kTLS send: OpenSSL encrypts the output.
void SetUserSpaceTlsTransport(ConnectionState& state) {
  state.corkable = true;
  state.transport = std::make_unique<TlsTransport>(TlsTransport::SslPtr{nullptr, &::SSL_free});
}
}  // namespace

TEST(ConnectionStateCorkTest, UserSpaceTlsOutputLongerThanOneRecordIsCorked) {
  static constexpr std::size_t kRecordSize = TlsTransport::kMaxRecordPlainSize;

  ConnectionState state;
  SetUserSpaceTlsTransport(state);
  EXPECT_TRUE(state.encryptsOutputInUserSpace());
  EXPECT_FALSE(state.needsCork(kRecordSize, false));
  EXPECT_TRUE(state.needsCork(kRecordSize + 1U, false));

  // HTTP/1 output.
  state.queueOutput(test::MakeMessageData(kRecordSize, 'a'));
  EXPECT_FALSE(state.flushNeedsCork());
  state.queueOutput(test::MakeMessageData(1, 'b'));
  EXPECT_TRUE(state.flushNeedsCork());

  // A file alone, read then encrypted by chunks.
  ConnectionState fileState;
  SetUserSpaceTlsTransport(fileState);
  EXPECT_TRUE(fileState.attachFilePayload(FilePayload{File{}, 0, kRecordSize}));
  EXPECT_FALSE(fileState.flushNeedsCork());
  fileState.tunnelOrFileBuffer.append(std::string_view("x"));
  EXPECT_TRUE(fileState.flushNeedsCork());

  // Protocol handler output.
  ConnectionState handlerState;
  SetUserSpaceTlsTransport(handlerState);
  handlerState.protocolHandler = std::make_unique<PendingOutputProtocolHandler>(kRecordSize);
  EXPECT_FALSE(handlerState.flushNeedsCork());
  handlerState.protocolHandler = std::make_unique<PendingOutputProtocolHandler>(kRecordSize + 1U);
  EXPECT_TRUE(handlerState.flushNeedsCork());

  // Not corkable.
  handlerState.corkable = false;
  EXPECT_FALSE(handlerState.flushNeedsCork());
}
#endif
