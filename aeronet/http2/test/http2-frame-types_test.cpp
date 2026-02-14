#include "aeronet/http2-frame-types.hpp"

#include <gtest/gtest.h>

namespace aeronet {

TEST(Http2FrameTypeTest, IsClientStream) {
  EXPECT_TRUE(http2::IsClientStream(1));
  EXPECT_TRUE(http2::IsClientStream(3));
  EXPECT_TRUE(http2::IsClientStream(2147483647));

  EXPECT_FALSE(http2::IsClientStream(0));
  EXPECT_FALSE(http2::IsClientStream(2));
  EXPECT_FALSE(http2::IsClientStream(4));
}

TEST(Http2FrameTypeTest, IsServerStream) {
  EXPECT_TRUE(http2::IsServerStream(2));
  EXPECT_TRUE(http2::IsServerStream(4));
  EXPECT_TRUE(http2::IsServerStream(2147483646));

  EXPECT_FALSE(http2::IsServerStream(0));
  EXPECT_FALSE(http2::IsServerStream(1));
  EXPECT_FALSE(http2::IsServerStream(3));
}

}  // namespace aeronet