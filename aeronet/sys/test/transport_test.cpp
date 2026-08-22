#include "aeronet/transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

TEST(PlainTransport, GatherWriteUsesWritevForAllFragments) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  BaseFd readFdGuard(fds[0]);
  BaseFd writeFdGuard(fds[1]);

  PlainTransport transport(fds[1], ZerocopyMode::Disabled, 0U);
  const std::array<std::string_view, 4> fragments{"HEAD", "", "BODY", "TAIL"};

  const auto result = transport.write(std::span<const std::string_view>(fragments));
  EXPECT_EQ(result.bytesProcessed, 12U);
  EXPECT_EQ(result.want, TransportHint::None);

  std::array<char, 16> received{};
  const auto bytesRead = ::read(fds[0], received.data(), received.size());
  ASSERT_EQ(bytesRead, 12);
  EXPECT_EQ(std::string_view(received.data(), 12), "HEADBODYTAIL");
}

TEST(PlainTransport, GatherWriteProcessesMoreThanOneSystemBatch) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  BaseFd readFdGuard(fds[0]);
  BaseFd writeFdGuard(fds[1]);

  PlainTransport transport(fds[1], ZerocopyMode::Disabled, 0U);
  std::array<std::string_view, 70> fragments;
  fragments.fill("x");

  const auto result = transport.write(std::span<const std::string_view>(fragments));
  EXPECT_EQ(result.bytesProcessed, fragments.size());
  EXPECT_EQ(result.want, TransportHint::None);

  std::array<char, 70> received{};
  const auto bytesRead = ::read(fds[0], received.data(), received.size());
  ASSERT_EQ(bytesRead, static_cast<int64_t>(received.size()));
  EXPECT_TRUE(std::ranges::all_of(received, [](char ch) { return ch == 'x'; }));
}

TEST(PlainTransport, GatherWriteTracksPartialProgressAcrossFragments) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  BaseFd readFdGuard(fds[0]);
  BaseFd writeFdGuard(fds[1]);

  test::SetWritevActions(fds[1], {IoAction{6, 0}, IoAction{-1, error::kWouldBlock}});

  PlainTransport transport(fds[1], ZerocopyMode::Disabled, 0U);
  const std::array<std::string_view, 3> fragments{"HEAD", "BODY", "TAIL"};

  const auto result = transport.write(std::span<const std::string_view>(fragments));
  EXPECT_EQ(result.bytesProcessed, 6U);
  EXPECT_EQ(result.want, TransportHint::WriteReady);
}
namespace {

// A minimal custom backend that provides only read/write, so Transport supplies the default capabilities.
class BaseDefaultsTransport final : public TransportBackend<BaseDefaultsTransport> {
 public:
  static TransportResult read(char* /*buf*/, std::size_t /*len*/) { return {0, TransportHint::None}; }

  static TransportResult write(std::string_view /*data*/) { return {0, TransportHint::None}; }
};

class OrderedDefaultsTransport final : public TransportBackend<OrderedDefaultsTransport> {
 public:
  explicit OrderedDefaultsTransport(bool shortFirstWrite) : _shortFirstWrite(shortFirstWrite) {}

  static TransportResult read(char* /*buf*/, std::size_t /*len*/) { return {0, TransportHint::None}; }

  TransportResult write(std::string_view data) {
    const std::size_t written =
        _shortFirstWrite && _writeCalls == 0 ? std::min<std::size_t>(2, data.size()) : data.size();
    _written.append(data.substr(0, written));
    ++_writeCalls;
    return {written, TransportHint::None};
  }

  [[nodiscard]] std::string_view written() const noexcept { return _written; }
  [[nodiscard]] std::size_t writeCalls() const noexcept { return _writeCalls; }

 private:
  std::string _written;
  std::size_t _writeCalls{0};
  bool _shortFirstWrite;
};

class CountingTransportBackend final : public TransportBackend<CountingTransportBackend> {
 public:
  explicit CountingTransportBackend(std::size_t& destructions) : _destructions(destructions) {}

  ~CountingTransportBackend() { ++_destructions; }

  static TransportResult read(char* /*buf*/, std::size_t /*len*/) { return {1, TransportHint::None}; }
  static TransportResult write(std::string_view data) { return {data.size(), TransportHint::None}; }

 private:
  std::size_t& _destructions;
};

class CapabilityTransportBackend final : public TransportBackend<CapabilityTransportBackend> {
 public:
  static TransportResult read(char* /*buf*/, std::size_t len) { return {len, TransportHint::None}; }
  static TransportResult write(std::string_view data) { return {data.size(), TransportHint::None}; }

  static TransportResult write(std::string_view firstBuf, std::string_view secondBuf) {
    return {firstBuf.size() + secondBuf.size(), TransportHint::None};
  }

  static TransportResult write(std::span<const std::string_view> buffers) {
    std::size_t size = 0;
    for (const std::string_view buffer : buffers) {
      size += buffer.size();
    }
    return {size, TransportHint::None};
  }

  static TransportResult sendFile(const File& /*file*/, std::size_t& offset, std::size_t count) {
    offset += count;
    return {count, TransportHint::None};
  }

  static std::size_t pollZerocopyCompletions() noexcept { return 7; }
  static bool supportsSendfile() noexcept { return true; }
  static bool handshakeDone() noexcept { return false; }
  static bool hasPendingReadData() noexcept { return true; }
  static bool isZerocopyEnabled() noexcept { return true; }
  static bool hasZerocopyPending() noexcept { return true; }

  void disableZerocopy() noexcept { _zerocopyDisabled = true; }
  [[nodiscard]] bool zerocopyDisabled() const noexcept { return _zerocopyDisabled; }

 private:
  bool _zerocopyDisabled{false};
};
}  // namespace

static_assert(!std::is_polymorphic_v<Transport>);
static_assert(!std::is_polymorphic_v<PlainTransport>);
#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(Transport) == 3 * sizeof(void*));
#endif
static_assert(sizeof(PlainTransport) == sizeof(SocketTransportState));

TEST(TransportTest, DefaultTransportIsEmpty) {
  Transport transport;
  EXPECT_FALSE(transport);
  EXPECT_EQ(transport.kind(), TransportKind::Empty);
  EXPECT_EQ(transport.get<PlainTransport>(), nullptr);
  EXPECT_EQ(transport.get<BaseDefaultsTransport>(), nullptr);
  EXPECT_FALSE(transport.isPlain());
  EXPECT_FALSE(transport.isTls());
}

TEST(TransportTest, SelfOperatorEqualDoesNothing) {
  Transport transport;
  auto& self = transport;
  self = std::move(transport);
  EXPECT_FALSE(self);
  EXPECT_EQ(self.kind(), TransportKind::Empty);
}

TEST(TransportTest, InlinePlainTransportMovesAndLeavesSourceEmpty) {
  Transport transport(-1, ZerocopyMode::Disabled, 0U);

  EXPECT_EQ(transport.kind(), TransportKind::Plain);
  EXPECT_TRUE(transport.isPlain());
  EXPECT_FALSE(transport.isTls());
  EXPECT_NE(transport.get<PlainTransport>(), nullptr);

  Transport moved(std::move(transport));
  EXPECT_FALSE(transport);                            // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(transport.kind(), TransportKind::Empty);  // NOLINT(bugprone-use-after-move)
  EXPECT_NE(moved.get<PlainTransport>(), nullptr);
  moved.reset();
  EXPECT_FALSE(moved);
}

TEST(TransportTest, ErasedCustomOwnershipAndBorrowingRespectLifetime) {
  std::size_t destructions = 0;
  CountingTransportBackend borrowedBackend(destructions);
  {
    Transport borrowed = Transport::Borrow(borrowedBackend);
    EXPECT_EQ(borrowed.kind(), TransportKind::Custom);
    EXPECT_FALSE(borrowed.isPlain());
    EXPECT_FALSE(borrowed.isTls());
    EXPECT_EQ(borrowed.get<CountingTransportBackend>(), &borrowedBackend);
    const Transport& constBorrowed = borrowed;
    EXPECT_EQ(constBorrowed.get<CountingTransportBackend>(), &borrowedBackend);
    EXPECT_EQ(borrowed.get<BaseDefaultsTransport>(), nullptr);
    EXPECT_EQ(borrowed.get<PlainTransport>(), nullptr);
    EXPECT_EQ(borrowed.write("abc").bytesProcessed, 3U);
    borrowed.reset();
    EXPECT_FALSE(borrowed);
    EXPECT_EQ(destructions, 0U);
  }
  EXPECT_EQ(destructions, 0U);

  {
    Transport owned(std::make_unique<CountingTransportBackend>(destructions));
    EXPECT_EQ(owned.kind(), TransportKind::Custom);
    owned.reset();
    EXPECT_FALSE(owned);
    EXPECT_EQ(destructions, 1U);
  }
  EXPECT_EQ(destructions, 1U);
}

TEST(TransportTest, MoveAssignmentReleasesThePreviousOwnedBackend) {
  std::size_t destructions = 0;
  Transport source(std::make_unique<CountingTransportBackend>(destructions));
  Transport destination(std::make_unique<CountingTransportBackend>(destructions));

  destination = std::move(source);
  EXPECT_FALSE(source);  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(destructions, 1U);

  destination.reset();
  EXPECT_EQ(destructions, 2U);
}

TEST(TransportTest, BaseTransportDoesNotSupportSendfile) {
  BaseDefaultsTransport backend;
  Transport transport = Transport::Borrow(backend);
  char buf{};
  EXPECT_EQ(transport.read(&buf, 1).bytesProcessed, 0U);
  EXPECT_TRUE(transport.handshakeDone());
  EXPECT_FALSE(transport.hasPendingReadData());
  EXPECT_FALSE(transport.isZerocopyEnabled());
  EXPECT_FALSE(transport.hasZerocopyPending());
  EXPECT_EQ(transport.pollZerocopyCompletions(), 0U);
  transport.disableZerocopy();
  EXPECT_FALSE(transport.supportsSendfile());
  File file;  // ignored by the default sendFile()
  std::size_t offset = 0;
  const auto res = transport.sendFile(file, offset, 0);
  EXPECT_EQ(res.bytesProcessed, 0U);
  EXPECT_EQ(res.want, TransportHint::Error);

  const auto writeResult = transport.write("HEAD", "BODY");
  EXPECT_EQ(writeResult.bytesProcessed, 0U);
  EXPECT_EQ(writeResult.want, TransportHint::None);
}

TEST(TransportTest, CustomBackendDispatchesProvidedOperations) {
  CapabilityTransportBackend backend;
  Transport transport = Transport::Borrow(backend);
  char buf{};
  EXPECT_EQ(transport.read(&buf, 3).bytesProcessed, 3U);
  EXPECT_EQ(transport.write("abc").bytesProcessed, 3U);
  EXPECT_EQ(transport.write("HEAD", "BODY").bytesProcessed, 8U);

  const std::array<std::string_view, 3> fragments{"A", "", "BC"};
  EXPECT_EQ(transport.write(std::span<const std::string_view>(fragments)).bytesProcessed, 3U);
  EXPECT_TRUE(transport.supportsSendfile());
  EXPECT_FALSE(transport.handshakeDone());
  EXPECT_TRUE(transport.hasPendingReadData());
  EXPECT_TRUE(transport.isZerocopyEnabled());
  EXPECT_TRUE(transport.hasZerocopyPending());
  EXPECT_EQ(transport.pollZerocopyCompletions(), 7U);

  File file;
  std::size_t offset = 2;
  EXPECT_EQ(transport.sendFile(file, offset, 5).bytesProcessed, 5U);
  EXPECT_EQ(offset, 7U);

  transport.disableZerocopy();
  EXPECT_TRUE(backend.zerocopyDisabled());
}

TEST(TransportTest, GatherFallbackPreservesOrderingAndStopsOnShortWrite) {
  const std::array<std::string_view, 3> fragments{"HEAD", "", "BODY"};

  OrderedDefaultsTransport complete(false);
  Transport completeBase = Transport::Borrow(complete);
  const auto completeResult = completeBase.write(std::span<const std::string_view>(fragments));
  EXPECT_EQ(completeResult.bytesProcessed, 8U);
  EXPECT_EQ(completeResult.want, TransportHint::None);
  EXPECT_EQ(complete.written(), "HEADBODY");
  EXPECT_EQ(complete.writeCalls(), 2U);

  OrderedDefaultsTransport partial(true);
  Transport partialBase = Transport::Borrow(partial);
  const auto partialResult = partialBase.write(std::span<const std::string_view>(fragments));
  EXPECT_EQ(partialResult.bytesProcessed, 2U);
  EXPECT_EQ(partialResult.want, TransportHint::None);
  EXPECT_EQ(partial.written(), "HE");
  EXPECT_EQ(partial.writeCalls(), 1U);

  OrderedDefaultsTransport completePair(false);
  Transport completePairBase = Transport::Borrow(completePair);
  const auto completePairResult = completePairBase.write("HEAD", "BODY");
  EXPECT_EQ(completePairResult.bytesProcessed, 8U);
  EXPECT_EQ(completePairResult.want, TransportHint::None);
  EXPECT_EQ(completePair.written(), "HEADBODY");
  EXPECT_EQ(completePair.writeCalls(), 2U);

  OrderedDefaultsTransport partialPair(true);
  Transport partialPairBase = Transport::Borrow(partialPair);
  const auto partialPairResult = partialPairBase.write("HEAD", "BODY");
  EXPECT_EQ(partialPairResult.bytesProcessed, 2U);
  EXPECT_EQ(partialPairResult.want, TransportHint::None);
  EXPECT_EQ(partialPair.written(), "HE");
  EXPECT_EQ(partialPair.writeCalls(), 1U);
}

TEST(PlainTransport, SupportsSendfile) {
  Transport transport(-1, ZerocopyMode::Disabled, 0U);
  EXPECT_TRUE(transport.supportsSendfile());
  EXPECT_TRUE(transport.handshakeDone());
  EXPECT_FALSE(transport.hasPendingReadData());
  EXPECT_FALSE(transport.isZerocopyEnabled());
  EXPECT_FALSE(transport.hasZerocopyPending());
  EXPECT_EQ(transport.pollZerocopyCompletions(), 0U);
  transport.disableZerocopy();
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
