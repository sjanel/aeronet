#include "../src/client-accept-encoding.hpp"

#include <gtest/gtest.h>

#include <cstring>

#if defined(AERONET_ENABLE_BROTLI) && defined(AERONET_ENABLE_ZLIB) && defined(AERONET_ENABLE_ZSTD)
#include <string_view>

#include "aeronet/http-constants.hpp"
#endif

namespace aeronet::internal::details {

TEST(ClientAcceptEncodingTest, ComputeAcceptEncodingSize) { EXPECT_GE(ComputeAcceptEncodingSize(), 0); }

TEST(ClientAcceptEncodingTest, MakeAcceptEncoding) {
  EXPECT_EQ(strlen(MakeAcceptEncoding().storage), ComputeAcceptEncodingSize());
#if defined(AERONET_ENABLE_BROTLI) && defined(AERONET_ENABLE_ZLIB) && defined(AERONET_ENABLE_ZSTD)
  EXPECT_NE(std::string_view(MakeAcceptEncoding().storage).find(http::zstd), std::string_view::npos);
#endif
}

}  // namespace aeronet::internal::details