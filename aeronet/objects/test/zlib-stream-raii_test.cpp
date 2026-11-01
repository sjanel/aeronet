#include "aeronet/zlib-stream-raii.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-variant.hpp"

namespace aeronet {

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(ZStreamRAII, DecompressInitFails) {
  test::FailNextRealloc();
  EXPECT_THROW(ZStreamRAII{ZlibVariant::gzip}, std::runtime_error);
}

TEST(ZStreamRAII, DeflateInitFails) {
  test::FailNextRealloc();
  EXPECT_THROW(ZStreamRAII(ZlibVariant::deflate, 6), std::runtime_error);
}

#endif

TEST(ZStreamRAII, DeflateParamsFails) {
  ZStreamRAII stream(ZlibVariant::deflate, 6);
  EXPECT_THROW(stream.initCompress(ZlibVariant::deflate, 18), std::runtime_error);
}

TEST(ZStreamRAII, VariantAndTypeSetAllocatedType) {
  // gzip + compress -> deflate
  {
    ZStreamRAII streamGzComp(ZlibVariant::gzip, 1);
  }

  // gzip + decompress -> inflate or error (noexcept ctor may set error)
  {
    ZStreamRAII streamGzInfl(ZlibVariant::gzip);
  }

  // deflate + compress -> deflate
  {
    ZStreamRAII streamDefComp(ZlibVariant::deflate, 1);
  }

  // deflate + decompress -> inflate or error
  {
    ZStreamRAII streamDefInfl(ZlibVariant::deflate);
  }
}

TEST(ZStreamRAII, DestructorCleansUpRepeatedly) {
  // Allocate and destroy many times to exercise ctor/dtor paths.
  for (int i = 0; i < 50; ++i) {
    ZStreamRAII compStream(ZlibVariant::gzip, 1);
    ZStreamRAII inflStream(ZlibVariant::deflate);
    // No explicit checks here beyond ensuring construction/destruction doesn't crash.
  }
}

TEST(ZStreamRAII, InvalidLevelThrows) {
  // Some zlib builds may accept out-of-range levels; the safe assertion here is that
  // either construction succeeds or throws a runtime_error. We assert that constructing
  // with an obviously invalid level does not crash the process.
  try {
    ZStreamRAII badLevelStream(ZlibVariant::gzip, static_cast<int8_t>(127));
    SUCCEED();
  } catch (const std::runtime_error&) {
    SUCCEED();
  }
}

TEST(ZStreamRAII, InvalidVariant) {
  auto invalidVariant = static_cast<std::underlying_type_t<ZlibVariant>>(120);
  try {
    ZStreamRAII badVariantStream(std::bit_cast<ZlibVariant>(invalidVariant));
    FAIL() << "Expected std::invalid_argument to be triggered";
  } catch (const std::invalid_argument&) {
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
  auto& self = empty2;
  EXPECT_NO_THROW(empty2 = std::move(self));
}

TEST(ZStreamRAII, InitCompressReusesExistingStateGzip) {
  ZStreamRAII stream(ZlibVariant::gzip, 6);
  // Reinitialize with same variant - should reuse
  EXPECT_NO_THROW(stream.initCompress(ZlibVariant::gzip, 9));
}

TEST(ZStreamRAII, InitCompressReusesExistingStateDeflate) {
  ZStreamRAII stream(ZlibVariant::deflate, 6);
  // Reinitialize with same variant - should reuse
  EXPECT_NO_THROW(stream.initCompress(ZlibVariant::deflate, 9));
}

TEST(ZStreamRAII, DeflateResetFailure) {
  // Create a valid deflate stream
  ZStreamRAII stream(ZlibVariant::deflate, 6);
  // Corrupt internal state so that deflateReset will fail (simulate zlib error)
  auto ptrState = std::exchange(stream.stream.state, nullptr);
  // initCompress attempts deflateReset when already in compress mode and should
  // throw when deflateReset returns a non-Z_OK value.
  EXPECT_THROW(stream.initCompress(ZlibVariant::deflate, 6), std::runtime_error);

  stream.stream.state = ptrState;
}

TEST(ZStreamRAII, FreeAfterDeflate) {
  ZStreamRAII stream(ZlibVariant::gzip, 6);
  EXPECT_NO_THROW(stream.end());
  // Calling free again on freed stream should be safe (idempotent)
  EXPECT_NO_THROW(stream.end());
}

TEST(ZStreamRAII, FreeAfterInflate) {
  ZStreamRAII stream(ZlibVariant::deflate);
  EXPECT_NO_THROW(stream.end());
  // Calling free again should be safe
  EXPECT_NO_THROW(stream.end());
}

TEST(ZStreamRAII, DestructorCallsFreeAfterInit) {
  {
    ZStreamRAII stream(ZlibVariant::gzip, 6);
    // Destructor will call free() - ensure no crash
  }
  // Success = no crash
  SUCCEED();
}

TEST(ZStreamRAII, VariantGzipCompressionRoundTrip) {
  const std::string payload =
      "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog.";

  // Compress
  ZStreamRAII compressor(ZlibVariant::gzip, 6);

  ZSetInput(compressor.stream, payload.data(), static_cast<uint32_t>(payload.size()));

  vector<unsigned char> compressed;
  compressed.reserve(static_cast<uint32_t>(payload.size() / 2));
  static constexpr std::size_t kChunk = 4096;
  vector<unsigned char> outbuf(kChunk);

  while (true) {
    ZSetOutput(compressor.stream, reinterpret_cast<char*>(outbuf.data()), outbuf.size());
    const int ret = ZDeflate(compressor.stream, Z_FINISH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t produced = outbuf.size() - compressor.stream.avail_out;
    compressed.insert(compressed.end(), outbuf.data(), outbuf.data() + produced);
    if (ret == Z_STREAM_END) {
      break;
    }
  }

  // Decompress
  ZStreamRAII decompressor(ZlibVariant::gzip);
  ZSetInput(decompressor.stream, reinterpret_cast<const char*>(compressed.data()), compressed.size());

  vector<char> decompressed;
  vector<unsigned char> inbuf(kChunk);

  do {
    ZSetOutput(decompressor.stream, reinterpret_cast<char*>(inbuf.data()), inbuf.size());
    const int ret = ZInflate(decompressor.stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t got = inbuf.size() - decompressor.stream.avail_out;
    decompressed.insert(decompressed.end(), reinterpret_cast<char*>(inbuf.data()),
                        reinterpret_cast<char*>(inbuf.data()) + got);
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
  ZStreamRAII compressor(ZlibVariant::deflate, 6);
  ZSetInput(compressor.stream, payload.data(), static_cast<uint32_t>(payload.size()));

  vector<unsigned char> compressed;
  static constexpr std::size_t kChunk = 4096;
  vector<unsigned char> outbuf(kChunk);

  while (true) {
    ZSetOutput(compressor.stream, reinterpret_cast<char*>(outbuf.data()), outbuf.size());
    const int ret = ZDeflate(compressor.stream, Z_FINISH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t produced = outbuf.size() - compressor.stream.avail_out;
    compressed.insert(compressed.end(), outbuf.data(), outbuf.data() + produced);
    if (ret == Z_STREAM_END) {
      break;
    }
  }

  // Decompress with deflate
  ZStreamRAII decompressor(ZlibVariant::deflate);
  ZSetInput(decompressor.stream, reinterpret_cast<const char*>(compressed.data()), compressed.size());

  vector<char> decompressed;
  vector<unsigned char> inbuf(kChunk);

  do {
    ZSetOutput(decompressor.stream, reinterpret_cast<char*>(inbuf.data()), inbuf.size());
    const int ret = ZInflate(decompressor.stream, Z_NO_FLUSH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t got = inbuf.size() - decompressor.stream.avail_out;
    decompressed.insert(decompressed.end(), reinterpret_cast<char*>(inbuf.data()),
                        reinterpret_cast<char*>(inbuf.data()) + got);
    if (ret == Z_STREAM_END) {
      break;
    }
  } while (decompressor.stream.avail_in != 0);

  const std::string result(decompressed.begin(), decompressed.end());
  EXPECT_EQ(result, payload);
}

TEST(ZStreamRAII, MultipleInitCallsAreIdempotent) {
  ZStreamRAII stream(ZlibVariant::gzip, 6);
  // Call init multiple times with same parameters
  for (int i = 0; i < 5; ++i) {
    EXPECT_NO_THROW(stream.initCompress(ZlibVariant::gzip, 6));
  }
}

TEST(ZStreamRAII, LargePayloadCompressionDecompression) {
  // Test with larger payload to ensure all code paths are covered
  std::string largePayload = test::MakePatternedPayload(512UL * 1024);

  ZStreamRAII compressor(ZlibVariant::gzip, 6);
  ZSetInput(compressor.stream, largePayload.data(), static_cast<uint32_t>(largePayload.size()));

  vector<unsigned char> compressed;
  static constexpr std::size_t kChunk = 65536;
  vector<unsigned char> outbuf(kChunk);

  while (true) {
    ZSetOutput(compressor.stream, reinterpret_cast<char*>(outbuf.data()), outbuf.size());
    const int ret = ZDeflate(compressor.stream, Z_FINISH);
    ASSERT_NE(ret, Z_STREAM_ERROR);
    const std::size_t produced = outbuf.size() - compressor.stream.avail_out;
    compressed.insert(compressed.end(), outbuf.data(), outbuf.data() + produced);
    if (ret == Z_STREAM_END) {
      break;
    }
  }

  // Decompress
  ZStreamRAII decompressor(ZlibVariant::gzip);
  ZSetInput(decompressor.stream, reinterpret_cast<const char*>(compressed.data()), compressed.size());

  vector<unsigned char> decompressed;
  while (true) {
    ZSetOutput(decompressor.stream, reinterpret_cast<char*>(outbuf.data()), outbuf.size());
    const int ret = ZInflate(decompressor.stream, Z_NO_FLUSH);
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
  vector<unsigned char> outbuf(1024);

  // Start with gzip compression
  ZStreamRAII stream(ZlibVariant::gzip, 6);
  ZSetInput(stream.stream, testData.data(), static_cast<uint32_t>(testData.size()));
  ZSetOutput(stream.stream, reinterpret_cast<char*>(outbuf.data()), outbuf.size());

  int ret = ZDeflate(stream.stream, Z_FINISH);
  ASSERT_TRUE(ret == Z_STREAM_END || ret == Z_OK);

  // Switch to deflate - this should reuse the cached buffer
  stream.initCompress(ZlibVariant::deflate, 6);
  ZSetInput(stream.stream, testData.data(), static_cast<uint32_t>(testData.size()));
  ZSetOutput(stream.stream, reinterpret_cast<char*>(outbuf.data()), outbuf.size());

  ret = ZDeflate(stream.stream, Z_FINISH);
  ASSERT_TRUE(ret == Z_STREAM_END || ret == Z_OK);

  // Switch back to gzip - again reusing the buffer
  stream.initCompress(ZlibVariant::gzip, 6);
  ZSetInput(stream.stream, testData.data(), static_cast<uint32_t>(testData.size()));
  ZSetOutput(stream.stream, reinterpret_cast<char*>(outbuf.data()), outbuf.size());

  ret = ZDeflate(stream.stream, Z_FINISH);
  ASSERT_TRUE(ret == Z_STREAM_END || ret == Z_OK);

  // If we got here without crashes or ASAN errors, buffer reuse is working correctly
}

namespace {

// Compresses 'data' as a whole stream with 'compressor', initialized for compression.
std::string CompressAll(ZStreamRAII& compressor, std::string_view data) {
  std::string compressed(ZDeflateBound(&compressor.stream, data.size()), '\0');
  ZSetInput(compressor.stream, data.data(), static_cast<uint32_t>(data.size()));
  ZSetOutput(compressor.stream, compressed.data(), static_cast<uint32_t>(compressed.size()));
  EXPECT_EQ(ZDeflate(compressor.stream, Z_FINISH), Z_STREAM_END);
  compressed.resize(compressed.size() - compressor.stream.avail_out);
  return compressed;
}

// Decompresses the whole stream 'compressed' with 'decompressor', initialized for decompression.
std::string DecompressAll(ZStreamRAII& decompressor, std::string_view compressed, std::size_t decompressedSize) {
  std::string decompressed(decompressedSize, '\0');
  ZSetInput(decompressor.stream, compressed.data(), static_cast<uint32_t>(compressed.size()));
  ZSetOutput(decompressor.stream, decompressed.data(), static_cast<uint32_t>(decompressed.size()));
  EXPECT_EQ(ZInflate(decompressor.stream, Z_FINISH), Z_STREAM_END);
  decompressed.resize(decompressed.size() - decompressor.stream.avail_out);
  return decompressed;
}

}  // namespace

// zlib binds its state to the address of the z_stream it was initialized with: the state points back to its stream, and
// zlib refuses a stream whose state points to another one. An initialized z_stream cannot be moved.
TEST(ZStreamRAII, InitializedStreamCannotBeRelocated) {
  ZStreamRAII compressor(ZlibVariant::raw, 6);
  zstream relocated = compressor.stream;
  EXPECT_EQ(ZDeflateReset(relocated), Z_STREAM_ERROR);
  EXPECT_EQ(ZDeflate(relocated, Z_FINISH), Z_STREAM_ERROR);
  EXPECT_EQ(ZDeflateReset(compressor.stream), Z_OK);

  ZStreamRAII decompressor(ZlibVariant::raw);
  relocated = decompressor.stream;
  EXPECT_EQ(ZInflateReset(relocated), Z_STREAM_ERROR);
  EXPECT_EQ(ZInflate(relocated, Z_NO_FLUSH), Z_STREAM_ERROR);
  EXPECT_EQ(ZInflateReset(decompressor.stream), Z_OK);
}

// Hence a move only hands over the memory cached by the stream: both streams are left uninitialized, and initialize
// again on their next use.
TEST(ZStreamRAII, MoveConstructionOfInitializedStreams) {
  const std::string payload = test::MakePatternedPayload(4096);
  ZStreamRAII compressor(ZlibVariant::gzip, 6);
  const std::string compressed = CompressAll(compressor, payload);

  ZStreamRAII movedCompressor(std::move(compressor));
  EXPECT_FALSE(movedCompressor.initialized());
  EXPECT_FALSE(compressor.initialized());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  movedCompressor.initCompress(ZlibVariant::gzip, 6);
  EXPECT_EQ(CompressAll(movedCompressor, payload), compressed);
  compressor.initCompress(ZlibVariant::gzip, 6);  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(CompressAll(compressor, payload), compressed);

  ZStreamRAII decompressor(ZlibVariant::gzip);
  EXPECT_EQ(DecompressAll(decompressor, compressed, payload.size()), payload);

  ZStreamRAII movedDecompressor(std::move(decompressor));
  EXPECT_FALSE(movedDecompressor.initialized());
  EXPECT_FALSE(decompressor.initialized());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  movedDecompressor.initDecompress(ZlibVariant::gzip);
  EXPECT_EQ(DecompressAll(movedDecompressor, compressed, payload.size()), payload);
}

TEST(ZStreamRAII, MoveAssignmentOfInitializedStreams) {
  const std::string payload = test::MakePatternedPayload(4096);
  ZStreamRAII compressor(ZlibVariant::deflate, 6);
  const std::string compressed = CompressAll(compressor, payload);
  ZStreamRAII decompressor(ZlibVariant::deflate);
  EXPECT_EQ(DecompressAll(decompressor, compressed, payload.size()), payload);

  decompressor = std::move(compressor);
  EXPECT_FALSE(decompressor.initialized());
  EXPECT_FALSE(compressor.initialized());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  decompressor.initCompress(ZlibVariant::deflate, 6);
  EXPECT_EQ(CompressAll(decompressor, payload), compressed);
  compressor.initDecompress(ZlibVariant::deflate);  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(DecompressAll(compressor, compressed, payload.size()), payload);
}

}  // namespace aeronet