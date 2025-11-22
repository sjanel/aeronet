#include "aeronet/transport.hpp"

#include <gtest/gtest.h>

using namespace aeronet;

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
