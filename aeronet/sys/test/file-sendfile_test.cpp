// Portable (Linux, macOS, Windows) checks that File copies, which duplicate the descriptor, can be sent with
// sendfile (TransmitFile on Windows) over a real TCP connection, in any order and as many times as needed: a duplicated
// descriptor shares its file position with the original one, but every transfer must start at its own offset.

#include "aeronet/file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/connection.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/zerocopy-mode.hpp"

#ifdef AERONET_POSIX
#include <sys/socket.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#endif

namespace aeronet {

namespace {

constexpr std::chrono::seconds kIoTimeout{10};
constexpr std::chrono::milliseconds kRetryPause{1};

// A region of a file to send.
struct FileRegion {
  const File& file;
  std::size_t offset;
  std::size_t length;
};

std::string MakePayload(std::size_t size) {
  std::string payload(size, '\0');
  for (std::size_t pos = 0; pos < size; ++pos) {
    payload[pos] = static_cast<char>('A' + ((pos * 7U) % 53U));
  }
  return payload;
}

// Reads exactly `size` bytes from the non-blocking socket fd, or less on EOF / error / timeout.
std::string ReceiveExactly(NativeHandle fd, std::size_t size) {
  PlainTransport transport(fd, ZerocopyMode::Disabled, 0U);
  std::string received(size, '\0');
  std::size_t receivedSize = 0;
  const auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
  while (receivedSize < size && std::chrono::steady_clock::now() < deadline) {
    const TransportResult res = transport.read(received.data() + receivedSize, size - receivedSize);
    receivedSize += res.bytesProcessed;
    if (res.bytesProcessed == 0) {
      if (res.want != TransportHint::ReadReady) {
        break;  // EOF or error
      }
      std::this_thread::sleep_for(kRetryPause);
    }
  }
  received.resize(receivedSize);
  return received;
}

// Sends the region with sendfile, waiting while the socket send buffer is full.
::testing::AssertionResult SendRegion(PlainTransport& transport, const FileRegion& region) {
  std::size_t offset = region.offset;
  std::size_t remaining = region.length;
  const auto deadline = std::chrono::steady_clock::now() + kIoTimeout;
  while (remaining != 0) {
    const TransportResult res = transport.sendFile(region.file, offset, remaining);
    if (res.bytesProcessed != 0) {
      remaining -= res.bytesProcessed;
      continue;
    }
    if (res.want != TransportHint::WriteReady) {
      return ::testing::AssertionFailure()
             << "sendFile failed at offset " << offset << " with " << remaining << " bytes remaining";
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return ::testing::AssertionFailure() << "sendFile timed out at offset " << offset;
    }
    std::this_thread::sleep_for(kRetryPause);
  }
  if (offset != region.offset + region.length) {
    return ::testing::AssertionFailure() << "sendFile advanced the offset to " << offset << " instead of "
                                         << region.offset + region.length;
  }
  return ::testing::AssertionSuccess();
}

// Sends the regions in order with sendfile over a fresh TCP loopback connection, and returns what the peer received.
std::string TransferOverLoopback(std::initializer_list<FileRegion> regions) {
  Socket listener(Socket::Type::Stream);
  uint16_t port = 0;
  listener.bindAndListen(false, port);

  char host[] = "127.0.0.1";
  ConnectResult connectResult = ConnectTCP(std::span<char>(host, sizeof(host) - 1U), port, 0, 5000);
  EXPECT_FALSE(connectResult.failure);
  EXPECT_FALSE(connectResult.connectPending);
  sockaddr_storage peerAddress{};
  Connection receiver(listener, peerAddress);
  EXPECT_TRUE(connectResult.cnx && receiver);
  if (!connectResult.cnx || !receiver) {
    return {};
  }

  std::size_t totalSize = 0;
  for (const FileRegion& region : regions) {
    totalSize += region.length;
  }

  // Read concurrently: a transfer larger than the socket buffers only completes while the peer drains them.
  std::optional<std::string> received;
  {
    std::jthread reader(
        [&received, fd = receiver.fd(), totalSize] { received.emplace(ReceiveExactly(fd, totalSize)); });
    PlainTransport sender(connectResult.cnx.fd(), ZerocopyMode::Disabled, 0U);
    for (const FileRegion& region : regions) {
      EXPECT_TRUE(SendRegion(sender, region));
    }
    // After a failed transfer, the reader gets EOF instead of waiting for the missing bytes until its timeout.
    connectResult.cnx.close();
  }
  return std::move(received).value_or(std::string{});
}

// Compares received bytes with the expected ones, reporting sizes and the first difference instead of whole payloads.
::testing::AssertionResult SameBytes(std::string_view actual, std::string_view expected) {
  if (actual == expected) {
    return ::testing::AssertionSuccess();
  }
  const auto [actualIt, expectedIt] = std::ranges::mismatch(actual, expected);
  return ::testing::AssertionFailure() << "received " << actual.size() << " bytes instead of " << expected.size()
                                       << ", first difference at offset " << (actualIt - actual.begin());
}

class FileSendfileTest : public ::testing::TestWithParam<std::size_t> {
 protected:
  FileSendfileTest() : _payload(MakePayload(GetParam())), _tmp(_tmpDir, _payload) {}

  [[nodiscard]] File openFile() const { return File(_tmp.filePath().string()); }

  std::string _payload;
  test::ScopedTempDir _tmpDir{"aeronet-file-sendfile"};
  test::ScopedTempFile _tmp;
};

// 4 KiB fits in the socket buffers, 1 MiB needs several (possibly pending) sendfile calls.
INSTANTIATE_TEST_SUITE_P(PayloadSizes, FileSendfileTest,
                         ::testing::Values(std::size_t{4096}, (std::size_t{1} << 20U) + 13U));

TEST_P(FileSendfileTest, OriginalFile) {
  const File original = openFile();
  ASSERT_TRUE(original);
  EXPECT_TRUE(SameBytes(TransferOverLoopback({{original, 0, original.size()}}), _payload));
}

TEST_P(FileSendfileTest, CopiedFile) {
  const File original = openFile();
  const File copy(original);  // NOLINT(performance-unnecessary-copy-initialization)
  ASSERT_TRUE(copy);
  EXPECT_TRUE(SameBytes(TransferOverLoopback({{copy, 0, copy.size()}}), _payload));
}

TEST_P(FileSendfileTest, CopyOfCopiedFile) {
  const File original = openFile();
  const File copy(original);    // NOLINT(performance-unnecessary-copy-initialization)
  const File copyOfCopy(copy);  // NOLINT(performance-unnecessary-copy-initialization)
  ASSERT_TRUE(copyOfCopy);
  EXPECT_TRUE(SameBytes(TransferOverLoopback({{copyOfCopy, 0, copyOfCopy.size()}}), _payload));
}

TEST_P(FileSendfileTest, CopyOutlivingOriginal) {
  std::optional<File> original(openFile());
  const File copy(*original);
  original.reset();
  ASSERT_TRUE(copy);
  EXPECT_TRUE(SameBytes(TransferOverLoopback({{copy, 0, copy.size()}}), _payload));
}

// Sending a file again must restart from the requested offset, not from where the previous transfer left the file
// position (TransmitFile on Windows works from the file position).
TEST_P(FileSendfileTest, SameFileTwice) {
  const File original = openFile();
  ASSERT_TRUE(original);
  EXPECT_TRUE(SameBytes(TransferOverLoopback({{original, 0, original.size()}, {original, 0, original.size()}}),
                        _payload + _payload));
}

TEST_P(FileSendfileTest, CopiedFileTwice) {
  const File original = openFile();
  const File copy(original);  // NOLINT(performance-unnecessary-copy-initialization)
  ASSERT_TRUE(copy);
  EXPECT_TRUE(SameBytes(TransferOverLoopback({{copy, 0, copy.size()}, {copy, 0, copy.size()}}), _payload + _payload));
}

// An original and its copy (sharing their file position on POSIX, where sendfile does not use it) must each send from
// their own requested offset, whatever the other one sent before.
TEST_P(FileSendfileTest, OriginalThenCopyThenOriginal) {
  const File original = openFile();
  const File copy(original);  // NOLINT(performance-unnecessary-copy-initialization)
  ASSERT_TRUE(copy);
  EXPECT_TRUE(SameBytes(
      TransferOverLoopback({{original, 0, original.size()}, {copy, 0, copy.size()}, {original, 0, original.size()}}),
      _payload + _payload + _payload));
}

// Same across separate connections, as an HTTP client sending the same file body request several times with keep-alive
// disabled does: each request copies the file, sends it, then closes the copy.
TEST_P(FileSendfileTest, FreshCopyPerConnection) {
  const File original = openFile();
  ASSERT_TRUE(original);
  for (int attempt = 0; attempt < 3; ++attempt) {
    const File copy(original);  // NOLINT(performance-unnecessary-copy-initialization)
    ASSERT_TRUE(copy);
    EXPECT_TRUE(SameBytes(TransferOverLoopback({{copy, 0, copy.size()}}), _payload)) << "attempt " << attempt;
  }
}

TEST_P(FileSendfileTest, RegionsOfCopiesInAnyOrder) {
  const File original = openFile();
  const File copy(original);  // NOLINT(performance-unnecessary-copy-initialization)
  ASSERT_TRUE(copy);
  const std::size_t half = _payload.size() / 2;
  const std::size_t tail = _payload.size() - half;
  const std::string_view payload(_payload);
  EXPECT_TRUE(SameBytes(
      TransferOverLoopback({{copy, half, tail}, {original, 0, half}, {copy, 1, 2}}),
      std::string(payload.substr(half)) + std::string(payload.substr(0, half)) + std::string(payload.substr(1, 2))));
}

// Reading a file (the TLS fallback reads it instead of using sendfile; on Windows, it moves the file position) must not
// shift what sendfile sends next, neither from that file nor from a copy.
TEST_P(FileSendfileTest, ReadAtThenSend) {
  const File original = openFile();
  const File copy(original);  // NOLINT(performance-unnecessary-copy-initialization)
  ASSERT_TRUE(copy);
  const std::string_view expectedTail = std::string_view(_payload).substr(_payload.size() / 2);
  for (const File* pReadFile : {&original, &copy}) {
    std::string readBuf(expectedTail.size(), '\0');
    ASSERT_EQ(pReadFile->readAt(std::as_writable_bytes(std::span<char>(readBuf)), _payload.size() - readBuf.size()),
              readBuf.size());
    EXPECT_TRUE(SameBytes(readBuf, expectedTail));
    EXPECT_TRUE(
        SameBytes(TransferOverLoopback({{original, 0, original.size()}, {copy, 0, copy.size()}}), _payload + _payload));
  }
}

// Copies used concurrently by several threads (a cached File copied into the responses of several server threads, say)
// must not disturb each other's transfers: on Windows, each copy reopens the file to get a file position of its own.
TEST_P(FileSendfileTest, CopiesSentConcurrentlyBySeveralThreads) {
  static constexpr std::size_t kNbThreads = 4;
  static constexpr std::size_t kNbTransfersPerThread = 3;
  const File original = openFile();
  ASSERT_TRUE(original);
  std::vector<std::string> received(kNbThreads * kNbTransfersPerThread);
  {
    std::vector<std::jthread> threads;
    threads.reserve(kNbThreads);
    for (std::size_t threadPos = 0; threadPos < kNbThreads; ++threadPos) {
      threads.emplace_back([&original, &received, threadPos] {
        const File copy(original);  // NOLINT(performance-unnecessary-copy-initialization)
        for (std::size_t transferPos = 0; transferPos < kNbTransfersPerThread; ++transferPos) {
          received[(threadPos * kNbTransfersPerThread) + transferPos] = TransferOverLoopback({{copy, 0, copy.size()}});
        }
      });
    }
  }
  for (std::size_t transferPos = 0; transferPos < received.size(); ++transferPos) {
    EXPECT_TRUE(SameBytes(received[transferPos], _payload)) << "transfer " << transferPos;
  }
}

}  // namespace

}  // namespace aeronet
