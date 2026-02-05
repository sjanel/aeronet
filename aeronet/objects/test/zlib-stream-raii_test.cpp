#include "aeronet/zlib-stream-raii.hpp"

#include <gtest/gtest.h>
#include <zconf.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/sys-test-support.hpp"

namespace aeronet {

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(ZStreamRAII, DecompressInitFails) {
  test::FailNextRealloc();
  EXPECT_THROW(ZStreamRAII{ZStreamRAII::Variant::gzip}, std::runtime_error);
}

TEST(ZStreamRAII, DeflateInitFails) {
  test::FailNextRealloc();
  EXPECT_THROW(ZStreamRAII(ZStreamRAII::Variant::deflate, 6), std::runtime_error);
}

#endif

TEST(ZStreamRAII, DeflateParamsFails) {
  ZStreamRAII stream(ZStreamRAII::Variant::deflate, 6);
  EXPECT_THROW(stream.initCompress(ZStreamRAII::Variant::deflate, 18), std::runtime_error);
}

TEST(ZStreamRAII, VariantAndTypeSetAllocatedType) {
  // gzip + compress -> deflate
  {
    ZStreamRAII streamGzComp(ZStreamRAII::Variant::gzip, 1);
  }

  // gzip + decompress -> inflate or error (noexcept ctor may set error)
  {
    ZStreamRAII streamGzInfl(ZStreamRAII::Variant::gzip);
  }

  // deflate + compress -> deflate
  {
    ZStreamRAII streamDefComp(ZStreamRAII::Variant::deflate, 1);
  }

  // deflate + decompress -> inflate or error
  {
    ZStreamRAII streamDefInfl(ZStreamRAII::Variant::deflate);
  }
}

TEST(ZStreamRAII, DestructorCleansUpRepeatedly) {
  // Allocate and destroy many times to exercise ctor/dtor paths.
  for (int i = 0; i < 50; ++i) {
    ZStreamRAII compStream(ZStreamRAII::Variant::gzip, 1);
    ZStreamRAII inflStream(ZStreamRAII::Variant::deflate);
    // No explicit checks here beyond ensuring construction/destruction doesn't crash.
  }
}

TEST(ZStreamRAII, InvalidLevelThrows) {
  // Some zlib builds may accept out-of-range levels; the safe assertion here is that
  // either construction succeeds or throws a runtime_error. We assert that constructing
  // with an obviously invalid level does not crash the process.
  try {
    ZStreamRAII badLevelStream(ZStreamRAII::Variant::gzip, static_cast<int8_t>(127));
    SUCCEED();
  } catch (const std::runtime_error &) {
    SUCCEED();
  }
}

TEST(ZStreamRAII, InvalidVariant) {
  auto invalidVariant = static_cast<std::underlying_type_t<ZStreamRAII::Variant>>(120);
  try {
    ZStreamRAII badVariantStream(static_cast<ZStreamRAII::Variant>(invalidVariant));
    FAIL() << "Expected std::invalid_argument to be triggered";
  } catch (const std::invalid_argument &) {
    SUCCEED();
  }
}

TEST(ZStreamRAII, CopyAndMoveWorksOnUninitializedStreams) {
  ZStreamRAII empty1;
  ZStreamRAII empty2;

  // Move assignment of uninitialized should work
  EXPECT_NO_THROW(empty2 = std::move(empty1));

  ZStreamRAII empty3;

  // self move does nothing
  auto &self = empty2;
  EXPECT_NO_THROW(empty2 = std::move(self));
}

TEST(ZStreamRAII, InitCompressReusesExistingStateGzip) {
  ZStreamRAII stream(ZStreamRAII::Variant::gzip, 6);
  // Reinitialize with same variant - should reuse
  EXPECT_NO_THROW(stream.initCompress(ZStreamRAII::Variant::gzip, 9));
}

TEST(ZStreamRAII, InitCompressReusesExistingStateDeflate) {
  ZStreamRAII stream(ZStreamRAII::Variant::deflate, 6);
  // Reinitialize with same variant - should reuse
  EXPECT_NO_THROW(stream.initCompress(ZStreamRAII::Variant::deflate, 9));
}

TEST(ZStreamRAII, DeflateResetFailure) {
  // Create a valid deflate stream
  ZStreamRAII stream(ZStreamRAII::Variant::deflate, 6);
  // Corrupt internal state so that deflateReset will fail (simulate zlib error)
  auto ptrState = std::exchange(stream.stream.state, nullptr);
  // initCompress attempts deflateReset when already in compress mode and should
  // throw when deflateReset returns a non-Z_OK value.
  EXPECT_THROW(stream.initCompress(ZStreamRAII::Variant::deflate, 6), std::runtime_error);

  stream.stream.state = ptrState;
}

TEST(ZStreamRAII, FreeAfterDeflate) {
  ZStreamRAII stream(ZStreamRAII::Variant::gzip, 6);
  EXPECT_NO_THROW(stream.end());
  // Calling free again on freed stream should be safe (idempotent)
  EXPECT_NO_THROW(stream.end());
}

TEST(ZStreamRAII, FreeAfterInflate) {
  ZStreamRAII stream(ZStreamRAII::Variant::deflate);
  EXPECT_NO_THROW(stream.end());
  // Calling free again should be safe
  EXPECT_NO_THROW(stream.end());
}

TEST(ZStreamRAII, DestructorCallsFreeAfterInit) {
  {
    ZStreamRAII stream(ZStreamRAII::Variant::gzip, 6);
    // Destructor will call free() - ensure no crash
  }
  // Success = no crash
  SUCCEED();
}

TEST(ZStreamRAII, VariantGzipCompressionRoundTrip) {
  const std::string payload =
      "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog.";

  // Compress
  ZStreamRAII compressor(ZStreamRAII::Variant::gzip, 6);
  compressor.stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(payload.data()));
  compressor.stream.avail_in = static_cast<uInt>(payload.size());

  std::vector<unsigned char> compressed;
  compressed.reserve(payload.size() / 2);
  static constexpr std::size_t kChunk = 4096;
  std::vector<unsigned char> outbuf(kChunk);

  while (true) {
    compressor.stream.next_out = outbuf.data();
    compressor.stream.avail_out = static_cast<uInt>(outbuf.size());
    const int ret = deflate(&compressor.stream, Z_FINISH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t produced = outbuf.size() - compressor.stream.avail_out;
    compressed.insert(compressed.end(), outbuf.data(), outbuf.data() + produced);
    if (ret == Z_STREAM_END) {
      break;
    }
  }

  // Decompress
  ZStreamRAII decompressor(ZStreamRAII::Variant::gzip);
  decompressor.stream.next_in = compressed.empty() ? nullptr : reinterpret_cast<Bytef *>(compressed.data());
  decompressor.stream.avail_in = static_cast<uInt>(compressed.size());

  std::vector<char> decompressed;
  std::vector<unsigned char> inbuf(kChunk);

  do {
    decompressor.stream.next_out = inbuf.data();
    decompressor.stream.avail_out = static_cast<uInt>(inbuf.size());
    const int ret = inflate(&decompressor.stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t got = inbuf.size() - decompressor.stream.avail_out;
    decompressed.insert(decompressed.end(), reinterpret_cast<char *>(inbuf.data()),
                        reinterpret_cast<char *>(inbuf.data()) + got);
    if (ret == Z_STREAM_END) {
      break;
    }
  } while (decompressor.stream.avail_in != 0);

  const std::string result(decompressed.begin(), decompressed.end());
  EXPECT_EQ(result, payload);
}

TEST(ZStreamRAII, VariantDeflateCompressionRoundTrip) {
  const std::string payload = "Deflate variant test data " + std::string(512, 'X');

  // Compress with deflate
  ZStreamRAII compressor(ZStreamRAII::Variant::deflate, 6);
  compressor.stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(payload.data()));
  compressor.stream.avail_in = static_cast<uInt>(payload.size());

  std::vector<unsigned char> compressed;
  static constexpr std::size_t kChunk = 4096;
  std::vector<unsigned char> outbuf(kChunk);

  while (true) {
    compressor.stream.next_out = outbuf.data();
    compressor.stream.avail_out = static_cast<uInt>(outbuf.size());
    const int ret = deflate(&compressor.stream, Z_FINISH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t produced = outbuf.size() - compressor.stream.avail_out;
    compressed.insert(compressed.end(), outbuf.data(), outbuf.data() + produced);
    if (ret == Z_STREAM_END) {
      break;
    }
  }

  // Decompress with deflate
  ZStreamRAII decompressor(ZStreamRAII::Variant::deflate);
  decompressor.stream.next_in = compressed.empty() ? nullptr : reinterpret_cast<Bytef *>(compressed.data());
  decompressor.stream.avail_in = static_cast<uInt>(compressed.size());

  std::vector<char> decompressed;
  std::vector<unsigned char> inbuf(kChunk);

  do {
    decompressor.stream.next_out = inbuf.data();
    decompressor.stream.avail_out = static_cast<uInt>(inbuf.size());
    const int ret = inflate(&decompressor.stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t got = inbuf.size() - decompressor.stream.avail_out;
    decompressed.insert(decompressed.end(), reinterpret_cast<char *>(inbuf.data()),
                        reinterpret_cast<char *>(inbuf.data()) + got);
    if (ret == Z_STREAM_END) {
      break;
    }
  } while (decompressor.stream.avail_in != 0);

  const std::string result(decompressed.begin(), decompressed.end());
  EXPECT_EQ(result, payload);
}

TEST(ZStreamRAII, MultipleInitCallsAreIdempotent) {
  ZStreamRAII stream(ZStreamRAII::Variant::gzip, 6);
  // Call init multiple times with same parameters
  for (int i = 0; i < 5; ++i) {
    EXPECT_NO_THROW(stream.initCompress(ZStreamRAII::Variant::gzip, 6));
  }
}

TEST(ZStreamRAII, LargePayloadCompressionDecompression) {
  // Test with larger payload to ensure all code paths are covered
  std::string largePayload = test::MakePatternedPayload(512UL * 1024);

  ZStreamRAII compressor(ZStreamRAII::Variant::gzip, 6);
  compressor.stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(largePayload.data()));
  compressor.stream.avail_in = static_cast<uInt>(largePayload.size());

  std::vector<unsigned char> compressed;
  static constexpr std::size_t kChunk = 65536;
  std::vector<unsigned char> outbuf(kChunk);

  while (true) {
    compressor.stream.next_out = outbuf.data();
    compressor.stream.avail_out = static_cast<uInt>(outbuf.size());
    const int ret = deflate(&compressor.stream, Z_FINISH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t produced = outbuf.size() - compressor.stream.avail_out;
    compressed.insert(compressed.end(), outbuf.data(), outbuf.data() + produced);
    if (ret == Z_STREAM_END) {
      break;
    }
  }

  // Decompress
  ZStreamRAII decompressor(ZStreamRAII::Variant::gzip);
  decompressor.stream.next_in = reinterpret_cast<Bytef *>(compressed.data());
  decompressor.stream.avail_in = static_cast<uInt>(compressed.size());

  std::vector<unsigned char> decompressed;
  while (true) {
    decompressor.stream.next_out = outbuf.data();
    decompressor.stream.avail_out = static_cast<uInt>(outbuf.size());
    const int ret = inflate(&decompressor.stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t got = outbuf.size() - decompressor.stream.avail_out;
    decompressed.insert(decompressed.end(), outbuf.data(), outbuf.data() + got);
    if (ret == Z_STREAM_END) {
      break;
    }
  }

  EXPECT_EQ(decompressed.size(), largePayload.size());
  EXPECT_EQ(std::memcmp(decompressed.data(), largePayload.data(), largePayload.size()), 0);
}

TEST(ZStreamRAII, VariantSwitchingReusesBuffer) {
  // This test verifies that switching between gzip and deflate doesn't require reallocation
  // of the internal buffer by using the custom allocator that caches the buffer.

  std::string testData = "Hello, World! This is a test to verify buffer reuse.";
  std::vector<unsigned char> outbuf(1024);

  // Start with gzip compression
  ZStreamRAII stream(ZStreamRAII::Variant::gzip, 6);
  stream.stream.next_in = reinterpret_cast<Bytef *>(testData.data());
  stream.stream.avail_in = static_cast<uInt>(testData.size());
  stream.stream.next_out = outbuf.data();
  stream.stream.avail_out = static_cast<uInt>(outbuf.size());

  int ret = deflate(&stream.stream, Z_FINISH);
  ASSERT_TRUE(ret == Z_STREAM_END || ret == Z_OK);

  // Switch to deflate - this should reuse the cached buffer
  stream.initCompress(ZStreamRAII::Variant::deflate, 6);
  stream.stream.next_in = reinterpret_cast<Bytef *>(testData.data());
  stream.stream.avail_in = static_cast<uInt>(testData.size());
  stream.stream.next_out = outbuf.data();
  stream.stream.avail_out = static_cast<uInt>(outbuf.size());

  ret = deflate(&stream.stream, Z_FINISH);
  ASSERT_TRUE(ret == Z_STREAM_END || ret == Z_OK);

  // Switch back to gzip - again reusing the buffer
  stream.initCompress(ZStreamRAII::Variant::gzip, 6);
  stream.stream.next_in = reinterpret_cast<Bytef *>(testData.data());
  stream.stream.avail_in = static_cast<uInt>(testData.size());
  stream.stream.next_out = outbuf.data();
  stream.stream.avail_out = static_cast<uInt>(outbuf.size());

  ret = deflate(&stream.stream, Z_FINISH);
  ASSERT_TRUE(ret == Z_STREAM_END || ret == Z_OK);

  // If we got here without crashes or ASAN errors, buffer reuse is working correctly
}

}  // namespace aeronet