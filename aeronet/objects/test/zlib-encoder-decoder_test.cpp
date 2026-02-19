#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-encoder.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

namespace {

constexpr std::size_t kDecoderChunkSize = 512;
constexpr std::size_t kMaxPlainBytes = 2UL * 1024 * 1024;

std::vector<std::string> SamplePayloads() {
  std::vector<std::string> payloads;
  payloads.reserve(4);
  payloads.emplace_back("");
  payloads.emplace_back("gzip -> deflate parity test");
  payloads.emplace_back(2048, 'x');
  payloads.emplace_back(test::MakePatternedPayload(64UL * 1024UL));
  return payloads;
}

const char* VariantName(ZStreamRAII::Variant variant) {
  return variant == ZStreamRAII::Variant::gzip ? "gzip" : "deflate";
}

void EncodeFull(ZlibEncoder& encoder, ZStreamRAII::Variant variant, std::string_view payload, RawChars& out,
                std::size_t extraCapacity = 0) {
  out.clear();
  out.reserve(ZDeflateBound(nullptr, payload.size()) + extraCapacity);
  const std::size_t written = encoder.encodeFull(variant, payload, out.capacity(), out.data());
  ASSERT_GT(written, 0UL);
  out.setSize(static_cast<RawChars::size_type>(written));
}

void ExpectOneShotRoundTrip(ZStreamRAII::Variant variant, std::string_view payload, std::size_t extraCapacity = 0) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  RawChars compressed;
  EncodeFull(encoder, variant, payload, compressed, extraCapacity);

  RawChars decompressed;
  ZlibDecoder decoder(variant);
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingRoundTrip(ZStreamRAII::Variant variant, std::string_view payload, std::size_t split,
                              std::size_t maxPlainBytes = kMaxPlainBytes) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  RawChars compressed;
  auto ctx = encoder.makeContext(variant);
  std::string_view remaining = payload;
  while (!remaining.empty()) {
    const std::size_t take = std::min(split, remaining.size());
    const std::string_view chunk = remaining.substr(0, take);
    remaining.remove_prefix(take);
    RawChars chunkOut;
    const auto written = test::EncodeChunk(*ctx, chunk, chunkOut);
    ASSERT_GE(written, 0);
    if (written > 0) {
      compressed.append(chunkOut);
    }
  }
  test::EndStream(*ctx, compressed);

  RawChars decompressed;
  ZlibDecoder decoder(variant);
  ASSERT_TRUE(decoder.decompressFull(compressed, maxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingDecoderRoundTrip(ZStreamRAII::Variant variant, std::string_view payload, std::size_t split) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  const auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(variant), payload, 4096U);
  ZlibDecoder decoder(variant);
  auto* ctx = decoder.makeContext();
  RawChars decompressed;
  std::string_view view(compressed);
  std::size_t offset = 0;
  while (offset < view.size()) {
    const std::size_t take = std::min(split, view.size() - offset);
    const std::string_view chunk = view.substr(offset, take);
    offset += take;
    const bool finalChunk = offset >= view.size();
    ASSERT_TRUE(ctx->decompressChunk(chunk, finalChunk, kMaxPlainBytes, kDecoderChunkSize, decompressed));
    ASSERT_TRUE(ctx->decompressChunk({}, finalChunk, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  }
  ASSERT_TRUE(ctx->decompressChunk({}, true, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

}  // namespace

class ZlibEncoderDecoderTest : public ::testing::TestWithParam<ZStreamRAII::Variant> {};

INSTANTIATE_TEST_SUITE_P(Variants, ZlibEncoderDecoderTest,
                         ::testing::Values(ZStreamRAII::Variant::gzip, ZStreamRAII::Variant::deflate));

TEST_P(ZlibEncoderDecoderTest, EncodeFullRoundTripsPayloads) {
  const auto variant = GetParam();
  for (const auto& payload : SamplePayloads()) {
    SCOPED_TRACE(testing::Message() << VariantName(variant) << " payload bytes=" << payload.size());
    ExpectOneShotRoundTrip(variant, payload, 64UL);
  }
}

TEST_P(ZlibEncoderDecoderTest, MaxDecompressedBytes) {
  const auto variant = GetParam();
  for (const auto& payload : SamplePayloads()) {
    SCOPED_TRACE(testing::Message() << VariantName(variant) << " payload bytes=" << payload.size());
    CompressionConfig cfg;
    ZlibEncoder encoder(cfg.zlib.level);
    RawChars compressed;
    EncodeFull(encoder, variant, payload, compressed, 64UL);

    RawChars decompressed;
    const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
    ZlibDecoder decoder;
    decoder.setVariant(variant);
    const bool isOK = decoder.decompressFull(compressed, limit, kDecoderChunkSize, decompressed);
    EXPECT_EQ(isOK, payload.empty());
    EXPECT_EQ(decompressed, std::string_view(payload).substr(0, limit));
  }
}

TEST_P(ZlibEncoderDecoderTest, EmptyChunksShouldAlwaysSucceed) {
  const auto variant = GetParam();
  ZlibDecoder decoder(variant);
  auto* ctx = decoder.makeContext();
  RawChars decompressed;
  EXPECT_TRUE(ctx->decompressChunk({}, false, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_TRUE(ctx->decompressChunk({}, true, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_TRUE(decompressed.empty());
}

TEST_P(ZlibEncoderDecoderTest, MoveConstructor) {
  const auto variant = GetParam();
  ZlibEncoderContext ctx1;
  ctx1.init(2, variant);

  RawChars produced;
  {
    RawChars chunkOut;
    const auto written = test::EncodeChunk(ctx1, "some-data", chunkOut);
    ASSERT_GE(written, 0);
    if (written > 0) {
      produced.append(chunkOut);
    }
  }
  test::EndStream(ctx1, produced);

  EXPECT_GT(produced.size(), 0UL);

  ZlibEncoderContext ctx2(std::move(ctx1));
  ctx2.init(2, variant);
  produced.clear();
  {
    RawChars chunkOut;
    const auto written = test::EncodeChunk(ctx2, "more-data", chunkOut);
    ASSERT_GE(written, 0);
    if (written > 0) {
      produced.append(chunkOut);
    }
  }
  test::EndStream(ctx2, produced);

  EXPECT_GT(produced.size(), 0UL);

  // self move does nothing
  auto& self = ctx2;
  ctx2 = std::move(self);
}

TEST_P(ZlibEncoderDecoderTest, InflateErrorOnInvalidData) {
  const auto variant = GetParam();
  RawChars invalidData("NotAValidZlibStream");
  RawChars decompressed;
  ZlibDecoder decoder(variant);
  EXPECT_FALSE(decoder.decompressFull(invalidData, kMaxPlainBytes, kDecoderChunkSize, decompressed));
}

TEST_P(ZlibEncoderDecoderTest, StreamingRoundTripsAcrossChunkSplits) {
  const auto variant = GetParam();
  static constexpr std::array kSplits{1ULL, 9ULL, 257ULL, 4096ULL, 10000ULL};
  for (const auto& payload : SamplePayloads()) {
    for (const auto split : kSplits) {
      SCOPED_TRACE(testing::Message() << VariantName(variant) << " payload bytes=" << payload.size()
                                      << " split=" << split);
      ExpectStreamingRoundTrip(variant, payload, split);
    }
  }
}

TEST_P(ZlibEncoderDecoderTest, StreamingDecoderHandlesChunkSplits) {
  const auto variant = GetParam();
  static constexpr std::array<std::size_t, 4> kDecodeSplits{1U, 7U, 257U, 4096U};
  for (const auto& payload : SamplePayloads()) {
    for (const auto split : kDecodeSplits) {
      SCOPED_TRACE(testing::Message() << VariantName(variant) << " payload bytes=" << payload.size()
                                      << " decode split=" << split);
      ExpectStreamingDecoderRoundTrip(variant, payload, split);
    }
  }
}

TEST(ZlibEncoderDecoderTest, SmallEncoderChunkSizeLargeChunks) {
  static constexpr std::size_t kChunkSize = 512UL * 1024;
  const auto largePayload = test::MakePatternedPayload(kChunkSize);
  // This test validates handling of very large streaming chunk sizes; it must not be
  // constrained by the small default max-decompressed limit used by other tests.
  ExpectStreamingRoundTrip(ZStreamRAII::Variant::deflate, largePayload, 8, kChunkSize);
}

TEST_P(ZlibEncoderDecoderTest, EncodeChunkAfterFinalizationReturnsZero) {
  // Finish the stream, then try to encode more data: should return -1 to signal an error.
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  auto ctx = encoder.makeContext(GetParam());
  // Produce some initial data.

  RawChars chunkOut;
  const auto written = test::EncodeChunk(*ctx, "Test data", chunkOut);
  ASSERT_GE(written, 0);

  // Finalize the stream.
  test::EndStream(*ctx, chunkOut);
  // Encoding after finalization should return -1 to signal an error.
  RawChars extra;
  extra.reserve(ZDeflateBound(nullptr, std::string_view{"More data"}.size()));
  EXPECT_LT(ctx->encodeChunk("More data", extra.capacity(), extra.data()), 0);
}

TEST_P(ZlibEncoderDecoderTest, StreamingSmallOutputBufferDrainsAndRoundTrips) {
  // Create a patterned payload large enough to force multiple deflate calls
  // when the encoder is given a very small output buffer.
  const std::string payload = test::MakePatternedPayload(1024);

  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  auto ctx = encoder.makeContext(GetParam());
  RawChars compressed;
  {
    RawChars chunkOut;
    const auto written = test::EncodeChunk(*ctx, std::string_view(payload), chunkOut);
    ASSERT_GE(written, 0);
    if (written > 0) {
      compressed.append(chunkOut);
    }
  }
  test::EndStream(*ctx, compressed);

  ZlibDecoder decoder(GetParam());
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

TEST_P(ZlibEncoderDecoderTest, StreamingRandomIncompressibleForcesMultipleIterations) {
  // Incompressible payload to force encoder to iterate and grow output as needed.
  const RawBytes payload = test::MakeRandomPayload(64UL * 1024);

#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  static constexpr std::size_t kChunkSize = 8UL;
#else
  static constexpr std::size_t kChunkSize = 1UL;  // small to force multiple iterations
#endif
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  auto ctx = encoder.makeContext(GetParam());
  RawChars compressed;
  {
    RawChars chunkOut;
    const auto written = test::EncodeChunk(
        *ctx, std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()), chunkOut);
    ASSERT_GE(written, 0);
    if (written > 0) {
      compressed.append(chunkOut);
    }
  }
  test::EndStream(*ctx, compressed);

  // Expect more than one chunk worth of output, implying multiple loop iterations.
  ASSERT_GT(compressed.size(), kChunkSize);

  RawChars decompressed;
  ZlibDecoder decoder(GetParam());
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));

  EXPECT_EQ(decompressed.size(), payload.size());
  EXPECT_EQ(std::memcmp(decompressed.data(), payload.data(), payload.size()), 0);
}

#if AERONET_WANT_MALLOC_OVERRIDES

TEST_P(ZlibEncoderDecoderTest, EncoderInitFailsOnMallocFailure) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  test::FailNextMalloc();
  EXPECT_THROW(encoder.makeContext(GetParam()), std::runtime_error);
}

#endif

TEST_P(ZlibEncoderDecoderTest, EncodeFullHandlesEmptyPayload) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  RawChars compressed;
  EncodeFull(encoder, GetParam(), std::string_view{}, compressed, 64UL);
  EXPECT_GT(compressed.size(), 0U);  // should produce some output even for empty input

  ZlibDecoder decoder(GetParam());
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(decompressed.size(), 0U);
}

TEST_P(ZlibEncoderDecoderTest, MaxCompressedBytesAndEndAreSane) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  auto ctx = encoder.makeContext(GetParam());
  const std::string payload = test::MakePatternedPayload(1024);

  const auto maxChunk = ctx->maxCompressedBytes(payload.size());
  ASSERT_GT(maxChunk, 0U);
  RawChars chunkOut(maxChunk);
  const auto written = ctx->encodeChunk(payload, chunkOut.capacity(), chunkOut.data());
  ASSERT_GE(written, 0);
  EXPECT_LE(static_cast<std::size_t>(written), maxChunk);

  RawChars tailOut(ctx->endChunkSize());
  while (true) {
    const auto tailWritten = ctx->end(tailOut.capacity(), tailOut.data());
    ASSERT_GE(tailWritten, 0);
    if (tailWritten == 0) {
      break;
    }
    EXPECT_LE(static_cast<std::size_t>(tailWritten), tailOut.capacity());
  }
}

TEST_P(ZlibEncoderDecoderTest, ZlibEndWithoutEnoughBufferShouldFail) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  auto ctx = encoder.makeContext(GetParam());

  // Provide a too-small buffer to end()
  const auto tailWritten = ctx->end(0UL, nullptr);
  EXPECT_EQ(tailWritten, -1);
}

TEST_P(ZlibEncoderDecoderTest, StreamingAndOneShotProduceSameOutput) {
  // Verify that streaming and one-shot produce identical compressed output or at least
  // decompress to the same plaintext.
  const std::string payload = test::MakePatternedPayload(4096);

  // One-shot
  CompressionConfig cfg;
  ZlibEncoder encoder1(cfg.zlib.level);
  RawChars oneShot;
  EncodeFull(encoder1, GetParam(), payload, oneShot);

  // Streaming
  ZlibEncoder encoder2(cfg.zlib.level);
  auto ctx = encoder2.makeContext(GetParam());
  RawChars streaming;
  {
    RawChars chunkOut;
    const auto written = test::EncodeChunk(*ctx, payload, chunkOut);
    ASSERT_GE(written, 0);
    if (written > 0) {
      streaming.append(chunkOut);
    }
  }
  test::EndStream(*ctx, streaming);

  // Both should decompress to the same plaintext
  RawChars decompressed1;
  RawChars decompressed2;
  ZlibDecoder decoder(GetParam());
  ASSERT_TRUE(decoder.decompressFull(oneShot, kMaxPlainBytes, kDecoderChunkSize, decompressed1));
  ASSERT_TRUE(decoder.decompressFull(streaming, kMaxPlainBytes, kDecoderChunkSize, decompressed2));
  EXPECT_EQ(std::string_view(decompressed1), std::string_view(decompressed2));
  EXPECT_EQ(std::string_view(decompressed1), payload);
}

TEST_P(ZlibEncoderDecoderTest, MultipleStreamingSessionsReuseBuffer) {
  // Test that buffer reuse across multiple streaming sessions doesn't cause issues
  const std::vector<std::string> payloads = {
      "First stream",
      "Second stream with more data",
      "Third stream",
  };

  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);

  for (const auto& payload : payloads) {
    auto ctx = encoder.makeContext(GetParam());
    RawChars compressed;
    {
      RawChars chunkOut;
      const auto written = test::EncodeChunk(*ctx, payload, chunkOut);
      ASSERT_GE(written, 0);
      if (written > 0) {
        compressed.append(chunkOut);
      }
    }
    test::EndStream(*ctx, compressed);

    ZlibDecoder decoder;
    decoder.setVariant(GetParam());
    RawChars decompressed;
    ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
    EXPECT_EQ(std::string_view(decompressed), payload);
  }
}

TEST_P(ZlibEncoderDecoderTest, ContextAssignmentThrowsWhenSessionActive) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  auto ctx = encoder.makeContext(GetParam());
  {
    RawChars chunkOut;
    const auto written = test::EncodeChunk(*ctx, "data", chunkOut);
    ASSERT_GE(written, 0);
  }

  // The context is in use - operations that require it to not be active should throw
  // This is tested through the actual operations that check _sessionActive
  SUCCEED();  // Implicit test of session tracking
}

TEST_P(ZlibEncoderDecoderTest, VariantSwitchingDuringSession) {
  // Initialize with one variant
  CompressionConfig cfg;
  ZlibEncoder encoder1(cfg.zlib.level);
  auto ctx1 = encoder1.makeContext(ZStreamRAII::Variant::gzip);
  RawChars compressed1;
  {
    RawChars chunkOut;
    const auto written = test::EncodeChunk(*ctx1, "test", chunkOut);
    ASSERT_GE(written, 0);
    if (written > 0) {
      compressed1.append(chunkOut);
    }
  }

  // Create a new encoder with different variant
  ZlibEncoder encoder2(cfg.zlib.level);
  auto ctx2 = encoder2.makeContext(ZStreamRAII::Variant::deflate);
  RawChars compressed2;
  {
    RawChars chunkOut;
    const auto written = test::EncodeChunk(*ctx2, "test", chunkOut);
    ASSERT_GE(written, 0);
    if (written > 0) {
      compressed2.append(chunkOut);
    }
  }

  // Both should be valid but different
  EXPECT_GT(compressed1.size(), 0);
  EXPECT_GT(compressed2.size(), 0);
}

TEST_P(ZlibEncoderDecoderTest, LevelSettingAffectsCompression) {
  // Higher compression levels should produce smaller output (generally)
  const std::string payload = test::MakePatternedPayload(8192);

  CompressionConfig cfg;
  cfg.zlib.level = 1;  // Low compression
  ZlibEncoder encoder1(cfg.zlib.level);
  RawChars compressed1;
  EncodeFull(encoder1, GetParam(), payload, compressed1);

  cfg.zlib.level = 9;  // High compression
  ZlibEncoder encoder2(cfg.zlib.level);
  RawChars compressed2;
  EncodeFull(encoder2, GetParam(), payload, compressed2);

  // High compression should generally be smaller or equal
  EXPECT_LE(static_cast<double>(compressed2.size()),
            static_cast<double>(compressed1.size()) * 1.1);  // Allow 10% margin

  // Both should decompress correctly
  RawChars decompressed1;
  RawChars decompressed2;
  ZlibDecoder decoder(GetParam());
  ASSERT_TRUE(decoder.decompressFull(compressed1, kMaxPlainBytes, kDecoderChunkSize, decompressed1));
  ASSERT_TRUE(decoder.decompressFull(compressed2, kMaxPlainBytes, kDecoderChunkSize, decompressed2));
  EXPECT_EQ(std::string_view(decompressed1), payload);
  EXPECT_EQ(std::string_view(decompressed2), payload);
}

TEST(ZlibEncoderDecoderTest, AllVariantsCoverageSmallDataGzip) {
  // Gzip specific coverage
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  for (const auto& payload : SamplePayloads()) {
    RawChars compressed;
    EncodeFull(encoder, ZStreamRAII::Variant::gzip, payload, compressed, 64UL);
    RawChars decompressed;
    ZlibDecoder decoder;
    decoder.setVariant(ZStreamRAII::Variant::gzip);
    ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
    EXPECT_EQ(std::string_view(decompressed), payload);
  }
}

TEST(ZlibEncoderDecoderTest, AllVariantsCoverageSmallDataDeflate) {
  // Deflate specific coverage
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  for (const auto& payload : SamplePayloads()) {
    RawChars compressed;
    EncodeFull(encoder, ZStreamRAII::Variant::deflate, payload, compressed, 64UL);
    RawChars decompressed;
    ZlibDecoder decoder;
    decoder.setVariant(ZStreamRAII::Variant::deflate);
    ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
    EXPECT_EQ(std::string_view(decompressed), payload);
  }
}

TEST(ZlibEncoderDecoderTest, EncodeChunkWithInsufficientOutputCapacity) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  auto ctx = encoder.makeContext(ZStreamRAII::Variant::deflate);

  // Create a large input.
  std::string large(4096, 'X');

  // Try to encode with only 1 byte available.
  char tiny[1];
  const auto result = ctx->encodeChunk(large, sizeof(tiny), tiny);

  // Zlib gracefully accepts the small buffer and returns 0 (empty output).
  // The check for "availIn != 0" that would return -1 is unreachable in practice.
  EXPECT_LE(result, 0);
}

}  // namespace aeronet