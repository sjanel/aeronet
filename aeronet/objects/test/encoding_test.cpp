#include "aeronet/encoding.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <type_traits>

#include "aeronet/decoder.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

static_assert(kNbContentEncodings == 5);
static_assert(GetEncodingStr(Encoding::zstd) == http::zstd);
static_assert(GetEncodingStr(Encoding::br) == http::br);
static_assert(GetEncodingStr(Encoding::gzip) == http::gzip);
static_assert(GetEncodingStr(Encoding::deflate) == http::deflate);
static_assert(GetEncodingStr(Encoding::none) == http::identity);

TEST(EncodingTest, GetEncodingStrReturnsExpectedConstants) {
  EXPECT_EQ(GetEncodingStr(Encoding::zstd), http::zstd);
  EXPECT_EQ(GetEncodingStr(Encoding::br), http::br);
  EXPECT_EQ(GetEncodingStr(Encoding::gzip), http::gzip);
  EXPECT_EQ(GetEncodingStr(Encoding::deflate), http::deflate);
  EXPECT_EQ(GetEncodingStr(Encoding::none), http::identity);
}

TEST(EncodingTest, AllEnumValuesHaveMapping) {
  // Verify that iterating over the declared range yields valid mappings
  for (std::underlying_type_t<Encoding> i = 0; i <= kNbContentEncodings; ++i) {
    const auto enc = static_cast<Encoding>(i);
    // The returned view must be non-empty
    EXPECT_FALSE(GetEncodingStr(enc).empty());
    EXPECT_NO_THROW(IsEncodingEnabled(enc));
  }
}

TEST(EncodingTest, IsEncodingEnabledReflectsBuildConfiguration) {
#ifdef AERONET_ENABLE_ZLIB
  EXPECT_TRUE(IsEncodingEnabled(Encoding::gzip));
  EXPECT_TRUE(IsEncodingEnabled(Encoding::deflate));
#else
  EXPECT_FALSE(IsEncodingEnabled(Encoding::gzip));
  EXPECT_FALSE(IsEncodingEnabled(Encoding::deflate));
#endif
#ifdef AERONET_ENABLE_ZSTD
  EXPECT_TRUE(IsEncodingEnabled(Encoding::zstd));
#else
  EXPECT_FALSE(IsEncodingEnabled(Encoding::zstd));
#endif
#ifdef AERONET_ENABLE_BROTLI
  EXPECT_TRUE(IsEncodingEnabled(Encoding::br));
#else
  EXPECT_FALSE(IsEncodingEnabled(Encoding::br));
#endif
  EXPECT_TRUE(IsEncodingEnabled(Encoding::none));

  EXPECT_FALSE(IsEncodingEnabled(static_cast<Encoding>(static_cast<std::underlying_type_t<Encoding>>(-1))));
}

class NonTriviallyDestructibleDecoder : public Decoder {
 public:
  NonTriviallyDestructibleDecoder() = default;
  ~NonTriviallyDestructibleDecoder() override {
    // non-trivial destructor
    ++_destructionCount;
  }

  bool decompressFull([[maybe_unused]] std::string_view input, [[maybe_unused]] std::size_t maxDecompressedBytes,
                      [[maybe_unused]] std::size_t decoderChunkSize, [[maybe_unused]] RawChars& out) override {
    return true;
  }

  std::unique_ptr<DecoderContext> makeContext() override { return {}; }

  static int destructionCount() { return _destructionCount; }

 private:
  static inline int _destructionCount = 0;
};

TEST(DecoderTest, MakeSureThatDestructorIsVirtual) {
  {
    std::unique_ptr<Decoder> dec = std::make_unique<NonTriviallyDestructibleDecoder>();
    // when dec goes out of scope, destructor must be called
  }
  EXPECT_EQ(NonTriviallyDestructibleDecoder::destructionCount(), 1);
}

}  // namespace aeronet
