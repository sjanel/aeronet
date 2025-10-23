#include <asm-generic/socket.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "base-fd.hpp"
#include "connection-state.hpp"
#include "file.hpp"
#include "transport.hpp"

using namespace aeronet;

namespace {
std::string createTempFileWithContent(const std::string& content) {
  char tmpl[] = "/tmp/aeronet_sendfile_test_XXXXXX";
  int fd = ::mkstemp(tmpl);
  if (fd < 0) {
    throw std::runtime_error("mkstemp failed");
  }
  BaseFd raii(fd);
  ssize_t nb = write(fd, content.data(), content.size());
  if (std::cmp_not_equal(nb, content.size())) {
    unlink(tmpl);
    throw std::runtime_error("write failed");
  }
  return tmpl;
}
}  // namespace

TEST(ConnectionStateSendfileTest, KernelSendfileSuccess) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(64UL * 1024, 'A');
  const std::string path = createTempFileWithContent(content);

  File file(path);
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

  unlink(path.c_str());
}

TEST(ConnectionStateSendfileTest, KernelSendfileWouldBlock) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  const std::string content(128UL * 1024, 'B');
  const std::string path = createTempFileWithContent(content);

  File file(path);
  ConnectionState state;
  state.fileSend.file = std::move(file);
  state.fileSend.offset = 0;
  state.fileSend.remaining = content.size();
  state.fileSend.active = true;

  // Make client socket non-blocking and intentionally small send buffer so the kernel
  // send buffer fills quickly and sendfile returns EAGAIN.
  int flags = fcntl(sv[0], F_GETFL, 0);
  ASSERT_GE(flags, 0);
  ASSERT_EQ(fcntl(sv[0], F_SETFL, flags | O_NONBLOCK), 0);

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

  unlink(path.c_str());
}

TEST(ConnectionStateSendfileTest, TlsSendfileLargeChunks) {
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd raii[] = {BaseFd(sv[0]), BaseFd(sv[1])};

  // Create a large file to force multiple chunks in the TLS path
  const std::size_t totalSize = (1 << 20);  // 1 MiB
  const std::string content(totalSize, 'T');
  const std::string path = createTempFileWithContent(content);

  File file(path);
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

  unlink(path.c_str());
}
