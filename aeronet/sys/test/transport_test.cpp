#include "aeronet/transport.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "aeronet/file.hpp"
#include "aeronet/system-error.hpp"

#define AERONET_WANT_READ_WRITE_OVERRIDES
#define AERONET_WANT_SENDFILE_PREAD_OVERRIDES

#include "aeronet/base-fd.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/zerocopy-mode.hpp"

#ifdef AERONET_POSIX
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aeronet {

TEST(TransportTest, ReadReturnsErrorWhenFdIsInvalid) {
  PlainTransport plainTransport(-1, ZerocopyMode::Disabled, 0U);  // invalid fd -> read should fail with EBADF
  char buf[16];
  const auto res = plainTransport.read(buf, sizeof(buf));
  EXPECT_EQ(res.bytesProcessed, 0U);
  EXPECT_EQ(res.want, TransportHint::Error);
}

TEST(TransportTest, WriteReturnsErrorWhenFdIsInvalid) {
  PlainTransport plainTransport(-1, ZerocopyMode::Disabled, 0U);  // invalid fd -> write should fail with EBADF
  const auto res = plainTransport.write("hello");
  // When a fatal error occurs the implementation leaves bytesProcessed
  // at the amount written so far (0) and sets want to Error.
  EXPECT_EQ(res.bytesProcessed, 0U);
  EXPECT_EQ(res.want, TransportHint::Error);
}

TEST(PlainTransport, ReadHandlesEINTRAndEAGAIN) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const int readFd = fds[0];
  const int writeFd = fds[1];

  BaseFd readFdGuard(readFd);
  BaseFd writeFdGuard(writeFd);

  // Install actions: error::kInterrupted, error::kWouldBlock, then error::kWouldBlock, then success
  test::SetReadActions(
      readFd, {IoAction{-1, error::kInterrupted}, IoAction{-1, error::kWouldBlock}, IoAction{-1, error::kWouldBlock}});

  PlainTransport transport(readFd, ZerocopyMode::Opportunistic, 0U);
  char buf[8]{};

  // First call: error::kInterrupted -> ReadReady
  auto r1 = transport.read(buf, sizeof(buf));
  EXPECT_EQ(r1.bytesProcessed, 0U);
  EXPECT_EQ(r1.want, TransportHint::ReadReady);

  // Second call: error::kWouldBlock -> ReadReady
  auto r2 = transport.read(buf, sizeof(buf));
  EXPECT_EQ(r2.bytesProcessed, 0U);
  EXPECT_EQ(r2.want, TransportHint::ReadReady);

  // Third call: error::kWouldBlock -> ReadReady
  auto r3 = transport.read(buf, sizeof(buf));
  EXPECT_EQ(r3.bytesProcessed, 0U);
  EXPECT_EQ(r3.want, TransportHint::ReadReady);

  // Write data to pipe for successful read
  const char* msg = "test";
  ASSERT_EQ(::write(writeFd, msg, 4), 4);

  // Fourth call: success, reads real data
  auto r4 = transport.read(buf, sizeof(buf));
  EXPECT_EQ(r4.bytesProcessed, 4U);
  EXPECT_EQ(r4.want, TransportHint::None);
  EXPECT_EQ(std::memcmp(buf, msg, 4), 0);
}

TEST(PlainTransport, WriteHandlesEAGAINAndSuccess) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  const int readFd = fds[0];
  const int writeFd = fds[1];

  BaseFd readFdGuard(readFd);
  BaseFd writeFdGuard(writeFd);

  // Simulate: error::kInterrupted (retried internally), EAGAIN, EWOULDBLOCK, then success
  test::SetWriteActions(
      writeFd, {IoAction{-1, error::kInterrupted}, IoAction{-1, error::kWouldBlock}, IoAction{-1, error::kWouldBlock}});

  PlainTransport transport(writeFd, ZerocopyMode::Enabled, 0U);
  EXPECT_FALSE(transport.hasPendingReadData());
  std::string_view data("foobar");

  // First write: error::kInterrupted is retried internally, then hits error::kWouldBlock -> WriteReady with 0 bytes
  // written
  auto w1 = transport.write(data);
  EXPECT_EQ(w1.bytesProcessed, 0U);
  EXPECT_EQ(w1.want, TransportHint::WriteReady);

  // Second write: error::kWouldBlock -> WriteReady
  auto w2 = transport.write(data);
  EXPECT_EQ(w2.bytesProcessed, 0U);
  EXPECT_EQ(w2.want, TransportHint::WriteReady);

  // Third write: success (real write to pipe)
  auto w3 = transport.write(data);
  EXPECT_EQ(w3.bytesProcessed, 6U);
  EXPECT_EQ(w3.want, TransportHint::None);

  // Verify data was written
  char buf[8]{};
  ASSERT_EQ(::read(readFd, buf, sizeof(buf)), 6);
  EXPECT_EQ(std::memcmp(buf, data.data(), 6), 0);
}

TEST(PlainTransport, TwoBufWriteReturnsEarlyWhenWritevNeedsRetry) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const int readFd = fds[0];
  const int writeFd = fds[1];

  BaseFd readFdGuard(readFd);
  BaseFd writeFdGuard(writeFd);

  // Simulate writev returning EAGAIN -> caller should
  // receive a result with want != None and no data written.
  test::SetWritevActions(writeFd, {IoAction{-1, error::kWouldBlock}});

  PlainTransport transport(writeFd, ZerocopyMode::Disabled, 0U);
  std::string_view head("HEAD");
  std::string_view body("BODY-BODY");

  auto res = transport.write(head, body);
  EXPECT_EQ(res.bytesProcessed, 0U);
  EXPECT_EQ(res.want, TransportHint::WriteReady);
}

TEST(PlainTransport, TwoBufWriteUsesWritevSuccessfully) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const int readFd = fds[0];
  const int writeFd = fds[1];

  BaseFd readFdGuard(readFd);
  BaseFd writeFdGuard(writeFd);

  PlainTransport transport(writeFd, ZerocopyMode::Disabled, 0U);
  std::string_view head("HEAD");
  std::string_view body("BODY");

  // Write both buffers using writev
  auto res = transport.write(head, body);
  EXPECT_EQ(res.bytesProcessed, head.size() + body.size());
  EXPECT_EQ(res.want, TransportHint::None);

  // Read back and verify data was written correctly as one contiguous write
  char buf[16]{};
  ASSERT_EQ(::read(readFd, buf, sizeof(buf)), static_cast<int64_t>(head.size() + body.size()));
  EXPECT_EQ(std::string_view(buf, head.size() + body.size()), "HEADBODY");
}

TEST(PlainTransport, TwoBufWriteHandlesPartialWrite) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const int readFd = fds[0];
  const int writeFd = fds[1];

  BaseFd readFdGuard(readFd);
  BaseFd writeFdGuard(writeFd);

  PlainTransport transport(writeFd, ZerocopyMode::Disabled, 0U);
  std::string_view head("HEAD");
  std::string_view body("BODY-DATA");

  // Simulate partial write: only 2 bytes on first call, then EAGAIN
  // This tests that partial progress is correctly reported
  test::SetWritevActions(writeFd, {IoAction{2, 0}, IoAction{-1, error::kWouldBlock}});

  auto res = transport.write(head, body);
  EXPECT_EQ(res.bytesProcessed, 2U);
  EXPECT_EQ(res.want, TransportHint::WriteReady);
}

namespace {

// A minimal transport that overrides nothing beyond the two pure-virtual I/O methods, so it inherits the
// ITransport defaults: no sendfile support, and a sendFile() that reports it is unsupported.
class BaseDefaultsTransport final : public ITransport {
 public:
  TransportResult read(char* /*buf*/, std::size_t /*len*/) override { return {0, TransportHint::None}; }
  TransportResult write(std::string_view /*data*/) override { return {0, TransportHint::None}; }
};

}  // namespace

TEST(TransportTest, BaseTransportDoesNotSupportSendfile) {
  BaseDefaultsTransport transport;
  EXPECT_FALSE(transport.supportsSendfile());
  File file;  // ignored by the default sendFile()
  std::size_t offset = 0;
  const auto res = transport.sendFile(file, offset, 0);
  EXPECT_EQ(res.bytesProcessed, 0U);
  EXPECT_EQ(res.want, TransportHint::Error);
}

TEST(PlainTransport, SupportsSendfile) {
  PlainTransport transport(-1, ZerocopyMode::Disabled, 0U);
  EXPECT_TRUE(transport.supportsSendfile());
}

TEST(PlainTransport, SendFileTransfersFileToSocketPeer) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd sendFd(sv[0]);
  BaseFd peerFd(sv[1]);

  static constexpr std::string_view kPayload = "sendfile-unit-payload";
  test::ScopedTempDir tmpDir("aeronet-transport-sendfile");
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(static_cast<bool>(file));

  PlainTransport transport(sv[0], ZerocopyMode::Disabled, 0U);
  std::size_t offset = 0;
  std::size_t remaining = file.size();
  while (remaining != 0) {
    const auto res = transport.sendFile(file, offset, remaining);
    ASSERT_EQ(res.want, TransportHint::None);
    ASSERT_GT(res.bytesProcessed, 0U);
    remaining -= res.bytesProcessed;
  }

  std::array<char, 64> buf{};
  const auto nbRead = ::read(sv[1], buf.data(), buf.size());
  ASSERT_EQ(nbRead, static_cast<int64_t>(kPayload.size()));
  EXPECT_EQ(std::string_view(buf.data(), kPayload.size()), kPayload);
}

TEST(PlainTransport, SendFileReportsWriteReadyOnWouldBlockAndRetriesOnEINTR) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd sendFd(sv[0]);
  BaseFd peerFd(sv[1]);

  test::ScopedTempDir tmpDir("aeronet-transport-sendfile-wb");
  test::ScopedTempFile tmp(tmpDir, std::string_view{"payload"});
  File file(tmp.filePath().string());
  ASSERT_TRUE(static_cast<bool>(file));

  PlainTransport transport(sv[0], ZerocopyMode::Disabled, 0U);

  // A full send buffer surfaces as EAGAIN -> WriteReady with no progress.
  test::SetSendfileActions(sv[0], {IoAction{-1, error::kWouldBlock}});
  std::size_t offset = 0;
  auto res = transport.sendFile(file, offset, file.size());
  EXPECT_EQ(res.bytesProcessed, 0U);
  EXPECT_EQ(res.want, TransportHint::WriteReady);

  // EINTR is retried internally, so the caller only observes the eventual success.
  test::SetSendfileActions(sv[0], {IoAction{-1, error::kInterrupted}, IoAction{4, 0}});
  auto res2 = transport.sendFile(file, offset, file.size());
  EXPECT_EQ(res2.bytesProcessed, 4U);
  EXPECT_EQ(res2.want, TransportHint::None);
}

TEST(PlainTransport, SendFileReportsErrorOnFatalFailureAndOnUnexpectedEof) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd sendFd(sv[0]);
  BaseFd peerFd(sv[1]);

  test::ScopedTempDir tmpDir("aeronet-transport-sendfile-err");
  test::ScopedTempFile tmp(tmpDir, std::string_view{"payload"});
  File file(tmp.filePath().string());
  ASSERT_TRUE(static_cast<bool>(file));

  PlainTransport transport(sv[0], ZerocopyMode::Disabled, 0U);

  // A fatal error (peer reset / broken pipe) surfaces as Error.
  test::SetSendfileActions(sv[0], {IoAction{-1, error::kBrokenPipe}});
  std::size_t offset = 0;
  auto res = transport.sendFile(file, offset, file.size());
  EXPECT_EQ(res.bytesProcessed, 0U);
  EXPECT_EQ(res.want, TransportHint::Error);

  // sendfile() returning 0 means the input file ended early (truncated) -> Error, never an infinite spin.
  test::SetSendfileActions(sv[0], {IoAction{0, 0}});
  auto res2 = transport.sendFile(file, offset, file.size());
  EXPECT_EQ(res2.bytesProcessed, 0U);
  EXPECT_EQ(res2.want, TransportHint::Error);
}

TEST(PlainTransport, TwoBufWriteRetriesOnEINTR) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  const int readFd = fds[0];
  const int writeFd = fds[1];

  BaseFd readFdGuard(readFd);
  BaseFd writeFdGuard(writeFd);

  // Simulate writev first returning error::kInterrupted, then succeed writing full payload
  // We emulate this by installing actions: first (-1, error::kInterrupted), then (total_bytes, 0)
  const std::string_view head("HEAD");
  const std::string_view body("BODY");
  const int64_t total = static_cast<int64_t>(head.size() + body.size());
  test::SetWritevActions(writeFd, {IoAction{-1, error::kInterrupted}, IoAction{total, 0}});

  PlainTransport transport(writeFd, ZerocopyMode::Disabled, 0U);
  auto res = transport.write(head, body);

  // After error::kInterrupted the transport should retry internally and eventually report full write
  EXPECT_EQ(res.bytesProcessed, static_cast<std::size_t>(total));
  EXPECT_EQ(res.want, TransportHint::None);

  // Note: the test support overrides return synthetic success values and do not
  // actually copy data into the fd. We therefore only verify reported progress.
}

}  // namespace aeronet