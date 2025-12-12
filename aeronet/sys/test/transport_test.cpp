#include "aeronet/transport.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#define AERONET_WANT_READ_WRITE_OVERRIDES

#include "aeronet/base-fd.hpp"
#include "aeronet/sys-test-support.hpp"

namespace aeronet {

TEST(TransportTest, ReadReturnsErrorWhenFdIsInvalid) {
  PlainTransport plainTransport(-1);  // invalid fd -> read should fail with EBADF
  char buf[16];
  const auto res = plainTransport.read(buf, sizeof(buf));
  EXPECT_EQ(res.bytesProcessed, 0U);
  EXPECT_EQ(res.want, TransportHint::Error);
}

TEST(TransportTest, WriteReturnsErrorWhenFdIsInvalid) {
  PlainTransport plainTransport(-1);  // invalid fd -> write should fail with EBADF
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

  // Install actions: EINTR, EAGAIN, then EWOULDBLOCK, then success
  test::SetReadActions(readFd, {IoAction{-1, EINTR}, IoAction{-1, EAGAIN}, IoAction{-1, EWOULDBLOCK}});

  PlainTransport transport(readFd);
  char buf[8]{};

  // First call: EINTR -> ReadReady
  auto r1 = transport.read(buf, sizeof(buf));
  EXPECT_EQ(r1.bytesProcessed, 0U);
  EXPECT_EQ(r1.want, TransportHint::ReadReady);

  // Second call: EAGAIN -> ReadReady
  auto r2 = transport.read(buf, sizeof(buf));
  EXPECT_EQ(r2.bytesProcessed, 0U);
  EXPECT_EQ(r2.want, TransportHint::ReadReady);

  // Third call: EWOULDBLOCK -> ReadReady
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

  // Simulate: EINTR (retried internally), EAGAIN, EWOULDBLOCK, then success
  test::SetWriteActions(writeFd, {IoAction{-1, EINTR}, IoAction{-1, EAGAIN}, IoAction{-1, EWOULDBLOCK}});

  PlainTransport transport(writeFd);
  std::string_view data("foobar");

  // First write: EINTR is retried internally, then hits EAGAIN -> WriteReady with 0 bytes written
  auto w1 = transport.write(data);
  EXPECT_EQ(w1.bytesProcessed, 0U);
  EXPECT_EQ(w1.want, TransportHint::WriteReady);

  // Second write: EWOULDBLOCK -> WriteReady
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

TEST(PlainTransport, TwoBufWriteReturnsEarlyWhenHeadNeedsRetry) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const int readFd = fds[0];
  const int writeFd = fds[1];

  BaseFd readFdGuard(readFd);
  BaseFd writeFdGuard(writeFd);

  // Simulate the head (firstBuf) write returning EAGAIN -> caller should
  // receive a result with want != None and no body written.
  test::SetWriteActions(writeFd, {IoAction{-1, EAGAIN}});

  PlainTransport transport(writeFd);
  std::string_view head("HEAD");
  std::string_view body("BODY-BODY");

  auto res = static_cast<ITransport&>(transport).write(head, body);
  EXPECT_EQ(res.bytesProcessed, 0U);
  EXPECT_EQ(res.want, TransportHint::WriteReady);
}

}  // namespace aeronet