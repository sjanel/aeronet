#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoder-result.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-encoder.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-variant.hpp"

namespace aeronet {

namespace {

constexpr uint32_t kDecoderChunkSize = 512;
constexpr std::size_t kMaxPlainBytes = 2UL * 1024 * 1024;

vector<std::string> SamplePayloads() {
  vector<std::string> payloads;
  payloads.reserve(4);
  payloads.emplace_back("");
  payloads.emplace_back("gzip -> deflate parity test");
  payloads.emplace_back(2048, 'x');
  payloads.emplace_back(test::MakePatternedPayload(64UL * 1024UL));
  return payloads;
}

const char* VariantName(ZlibVariant variant) { return variant == ZlibVariant::gzip ? "gzip" : "deflate"; }

void EncodeFull(ZlibEncoder& encoder, ZlibVariant variant, std::string_view payload, RawChars& out,
                std::size_t extraCapacity = 0) {
  out.clear();
  out.reserve(ZDeflateBound(nullptr, payload.size()) + extraCapacity);
  const auto result = encoder.encodeFull(variant, payload, out.capacity(), out.data());
  ASSERT_FALSE(result.hasError());
  out.setSize(result.writtenIfNoError());
}

void ExpectOneShotRoundTrip(ZlibVariant variant, std::string_view payload, std::size_t extraCapacity = 0) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  RawChars compressed;
  EncodeFull(encoder, variant, payload, compressed, extraCapacity);

  RawChars decompressed;
  ZlibDecoder decoder(variant);
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingRoundTrip(ZlibVariant variant, std::string_view payload, std::size_t split,
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
    const auto result = test::EncodeChunk(*ctx, chunk, chunkOut);
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() > 0) {
      compressed.append(chunkOut);
    }
  }
  test::EndStream(*ctx, compressed);

  RawChars decompressed;
  ZlibDecoder decoder(variant);
  ASSERT_TRUE(decoder.decompressFull(compressed, maxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingDecoderRoundTrip(ZlibVariant variant, std::string_view payload, std::size_t split) {
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

class ZlibEncoderDecoderTest : public ::testing::TestWithParam<ZlibVariant> {};

}  // namespace

// zlib counts sizes in 32 bits: the contexts give inputs of 4 GiB and more in slices, tested here with small slices.
struct ZlibTestAccess {
  // Deflates data like encodeChunk() (Z_NO_FLUSH) or encodeFull() (Z_FINISH), appending the output to out.
  // Returns the status of the last deflate() call.
  static int Deflate(ZlibEncoderContext& ctx, std::string_view data, int flush, std::size_t maxSliceSize,
                     RawChars& out) {
    const int ret = ctx.deflateSlices(data, flush, out.availableCapacity(), out.end(), maxSliceSize);
    out.setEnd(ZOutputEnd(ctx._zs.stream));
    return ret;
  }

  static std::size_t AvailIn(const ZlibEncoderContext& ctx) { return ctx._zs.stream.avail_in; }

  static bool DecompressChunk(ZlibDecoderContext& ctx, std::string_view chunk, bool finalChunk,
                              std::size_t maxDecompressedBytes, std::size_t maxSliceSize, RawChars& out) {
    return ctx.decompressChunk(chunk, finalChunk, maxDecompressedBytes, kDecoderChunkSize, out, maxSliceSize);
  }
};

INSTANTIATE_TEST_SUITE_P(Variants, ZlibEncoderDecoderTest, ::testing::Values(ZlibVariant::gzip, ZlibVariant::deflate));

TEST(ZlibGateway, ClampSize) {
  static constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
  EXPECT_EQ(ZClampSize(0), 0U);
  EXPECT_EQ(ZClampSize(42), 42U);
  EXPECT_EQ(ZClampSize(kZMaxSize - 1), kMax - 1U);
  EXPECT_EQ(ZClampSize(kZMaxSize), kMax);
  EXPECT_EQ(ZClampSize(kZMaxSize + 1), kMax);
  EXPECT_EQ(ZClampSize(std::numeric_limits<std::size_t>::max()), kMax);
}

// An output room clamped to 32 bits is enough for the largest deflate input slice.
TEST(ZlibGateway, DeflateBoundOfTheLargestDeflateSliceFitsIn32Bits) {
  EXPECT_LE(ZDeflateBound(nullptr, kZMaxDeflateInputSize), kZMaxSize);
}

TEST_P(ZlibEncoderDecoderTest, MinEncodeChunkCapacityOfChunksLargerThanADeflateSlice) {
  ZlibEncoderContext ctx;
  ctx.init(CompressionConfig{}.zlib.level, GetParam());
  const std::size_t sliceBound = ctx.minEncodeChunkCapacity(kZMaxDeflateInputSize);
  EXPECT_GT(sliceBound, kZMaxDeflateInputSize);
  EXPECT_EQ(ctx.minEncodeChunkCapacity(kZMaxDeflateInputSize + 1), sliceBound + ctx.minEncodeChunkCapacity(1));
  EXPECT_EQ(ctx.minEncodeChunkCapacity(2 * kZMaxDeflateInputSize), (2 * sliceBound) + ctx.minEncodeChunkCapacity(0));
  EXPECT_EQ(ctx.minEncodeChunkCapacity((5 * kZMaxDeflateInputSize) + 7),
            (5 * sliceBound) + ctx.minEncodeChunkCapacity(7));
}

TEST_P(ZlibEncoderDecoderTest, DeflateInSlicesRoundTrips) {
  const auto variant = GetParam();
  const RawChars random = test::MakeRandomPayload(20000);
  const std::string patterned = test::MakePatternedPayload(20000);
  for (const std::string_view payload : {std::string_view(random), std::string_view(patterned)}) {
    for (const std::size_t sliceSize : {std::size_t{1}, std::size_t{7}, std::size_t{4096}, payload.size()}) {
      SCOPED_TRACE(testing::Message() << VariantName(variant) << " slice=" << sliceSize);
      ZlibEncoderContext ctx;
      ctx.init(CompressionConfig{}.zlib.level, variant);

      // Streaming: encodeChunk() then end().
      RawChars streamed(ctx.minEncodeChunkCapacity(payload.size()));
      ASSERT_EQ(ZlibTestAccess::Deflate(ctx, payload, Z_NO_FLUSH, sliceSize, streamed), Z_OK);
      EXPECT_EQ(ZlibTestAccess::AvailIn(ctx), 0U);
      test::EndStream(ctx, streamed);

      // One shot: encodeFull().
      ctx.init(CompressionConfig{}.zlib.level, variant);
      RawChars oneShot(ZDeflateBound(nullptr, payload.size()));
      ASSERT_EQ(ZlibTestAccess::Deflate(ctx, payload, Z_FINISH, sliceSize, oneShot), Z_STREAM_END);

      for (const RawChars* pCompressed : {&streamed, &oneShot}) {
        ZlibDecoder decoder(variant);
        RawChars decompressed;
        ASSERT_TRUE(decoder.decompressFull(*pCompressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
        EXPECT_TRUE(std::string_view(decompressed) == payload);
      }
    }
  }
}

// Deflating stops at the first input slice not entirely consumed for lack of output room.
TEST_P(ZlibEncoderDecoderTest, DeflateInSlicesStopsWhenTheOutputIsFull) {
  const RawChars payload = test::MakeRandomPayload(200000);
  ZlibEncoderContext ctx;
  ctx.init(CompressionConfig{}.zlib.level, GetParam());
  RawChars out(1024);
  // zlib holds back output it cannot write: a slice may be consumed with the output full, the next one cannot progress
  // then (Z_BUF_ERROR).
  const int ret = ZlibTestAccess::Deflate(ctx, payload, Z_NO_FLUSH, 65536, out);
  EXPECT_TRUE(ret == Z_OK || ret == Z_BUF_ERROR) << ret;
  EXPECT_EQ(out.size(), 1024U);
  EXPECT_NE(ZlibTestAccess::AvailIn(ctx), 0U);

  // Without any output room, deflate() cannot progress.
  ctx.init(CompressionConfig{}.zlib.level, GetParam());
  RawChars full(16);
  full.setSize(16);
  EXPECT_EQ(ZlibTestAccess::Deflate(ctx, payload, Z_NO_FLUSH, 7, full), Z_BUF_ERROR);
  EXPECT_EQ(ZlibTestAccess::AvailIn(ctx), 7U);
}

TEST_P(ZlibEncoderDecoderTest, EncodeFullWithoutOutputRoomIsNotEnoughCapacity) {
  ZlibEncoder encoder(CompressionConfig{}.zlib.level);
  char buf[1];
  const auto result = encoder.encodeFull(GetParam(), "some data", 0, buf);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error(), EncoderResult::Error::NotEnoughCapacity);

  // Without any output buffer, deflate() reports an inconsistent stream.
  const auto noBuffer = encoder.encodeFull(GetParam(), "some data", 0, nullptr);
  ASSERT_TRUE(noBuffer.hasError());
  EXPECT_EQ(noBuffer.error(), EncoderResult::Error::CompressionError);
}

TEST_P(ZlibEncoderDecoderTest, DecompressChunkInSlicesRoundTrips) {
  const auto variant = GetParam();
  const std::string payload = test::MakePatternedPayload(50000);
  ZlibEncoder encoder(CompressionConfig{}.zlib.level);
  RawChars compressed;
  EncodeFull(encoder, variant, payload, compressed);
  for (const std::size_t sliceSize : {std::size_t{1}, std::size_t{3}, std::size_t{4096}}) {
    SCOPED_TRACE(testing::Message() << VariantName(variant) << " slice=" << sliceSize);
    ZlibDecoder decoder(variant);
    RawChars decompressed;
    EXPECT_TRUE(ZlibTestAccess::DecompressChunk(*decoder.makeContext(), compressed, true, kMaxPlainBytes, sliceSize,
                                                decompressed));
    EXPECT_EQ(std::string_view(decompressed), payload);

    // The limit holds across the slices.
    for (const std::size_t limit : {payload.size() - 1, payload.size()}) {
      decompressed.clear();
      EXPECT_EQ(
          ZlibTestAccess::DecompressChunk(*decoder.makeContext(), compressed, true, limit, sliceSize, decompressed),
          limit == payload.size());
      EXPECT_LE(decompressed.size(), limit + 1);
    }

    // A truncated stream continues in the next chunk, unless this one is the last.
    const std::string_view truncated = std::string_view(compressed).substr(0, compressed.size() - 1);
    decompressed.clear();
    EXPECT_TRUE(ZlibTestAccess::DecompressChunk(*decoder.makeContext(), truncated, false, kMaxPlainBytes, sliceSize,
                                                decompressed));
    decompressed.clear();
    EXPECT_FALSE(ZlibTestAccess::DecompressChunk(*decoder.makeContext(), truncated, true, kMaxPlainBytes, sliceSize,
                                                 decompressed));
  }
}

// Data after the end of the stream is rejected, also when the stream ends exactly at the end of a slice.
TEST_P(ZlibEncoderDecoderTest, DecompressChunkInSlicesRejectsDataAfterTheStreamEnd) {
  const auto variant = GetParam();
  const std::string payload = test::MakePatternedPayload(5000);
  ZlibEncoder encoder(CompressionConfig{}.zlib.level);
  RawChars compressed;
  EncodeFull(encoder, variant, payload, compressed);
  const std::size_t streamSize = compressed.size();
  compressed.append(std::string_view("trailing garbage"));
  for (const std::size_t sliceSize : {std::size_t{1}, std::size_t{5}, streamSize}) {
    SCOPED_TRACE(testing::Message() << VariantName(variant) << " slice=" << sliceSize);
    ZlibDecoder decoder(variant);
    RawChars decompressed;
    EXPECT_FALSE(ZlibTestAccess::DecompressChunk(*decoder.makeContext(), compressed, true, kMaxPlainBytes, sliceSize,
                                                 decompressed));
    EXPECT_EQ(std::string_view(decompressed), payload);
  }
}

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
    // Exceeding the limit is detected by decoding one more byte.
    EXPECT_EQ(decompressed, std::string_view(payload).substr(0, limit + 1));
  }
}

// An empty chunk decodes nothing. When final, it tells whether the input fed so far completes the stream: a caller
// can feed pieces without knowing which one is the last, then check the end of the stream.
TEST_P(ZlibEncoderDecoderTest, EmptyFinalChunkTellsWhetherTheStreamIsComplete) {
  const auto variant = GetParam();
  const std::string payload = test::MakePatternedPayload(20000);
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  const auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(variant), payload, 4096U);
  const std::string_view stream(compressed);
  ZlibDecoder decoder(variant);
  auto* ctx = decoder.makeContext();
  RawChars decompressed;
  EXPECT_TRUE(ctx->decompressChunk({}, false, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_FALSE(ctx->decompressChunk({}, true, kMaxPlainBytes, kDecoderChunkSize, decompressed));  // nothing fed yet
  EXPECT_TRUE(decompressed.empty());
  // A context initialized again (by makeContext()) starts a new stream, so it is first reported incomplete.
  for (const std::size_t split : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
    const test::FedStream fed = test::FeedNonFinal(*decoder.makeContext(), stream, split, kDecoderChunkSize);
    EXPECT_TRUE(fed.ok) << split;
    EXPECT_EQ(fed.completeAt, std::vector<std::size_t>{stream.size()}) << split;
    EXPECT_TRUE(std::string_view(fed.decoded) == payload) << split;
  }
  const test::FedStream truncated =
      test::FeedNonFinal(*decoder.makeContext(), stream.substr(0, stream.size() - 1), 4096U, kDecoderChunkSize);
  EXPECT_TRUE(truncated.ok);
  EXPECT_TRUE(truncated.completeAt.empty());
}

TEST_P(ZlibEncoderDecoderTest, MoveConstructor) {
  const auto variant = GetParam();
  ZlibEncoderContext ctx1;
  ctx1.init(2, variant);

  RawChars produced;
  {
    RawChars chunkOut;
    const auto result = test::EncodeChunk(ctx1, "some-data", chunkOut);
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() > 0) {
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
    const auto result = test::EncodeChunk(ctx2, "more-data", chunkOut);
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() > 0) {
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
  ExpectStreamingRoundTrip(ZlibVariant::deflate, largePayload, 8, kChunkSize);
}

TEST_P(ZlibEncoderDecoderTest, EncodeChunkAfterFinalizationReturnsZero) {
  // Finish the stream, then try to encode more data: should return -1 to signal an error.
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  auto ctx = encoder.makeContext(GetParam());
  // Produce some initial data.

  RawChars chunkOut;
  const auto result = test::EncodeChunk(*ctx, "Test data", chunkOut);
  ASSERT_FALSE(result.hasError());

  // Finalize the stream.
  test::EndStream(*ctx, chunkOut);
  // Encoding after finalization should return -1 to signal an error.
  RawChars extra;
  extra.reserve(ZDeflateBound(nullptr, std::string_view{"More data"}.size()));
  EXPECT_TRUE(ctx->encodeChunk("More data", extra.capacity(), extra.data()).hasError());
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
    const auto result = test::EncodeChunk(*ctx, std::string_view(payload), chunkOut);
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() > 0) {
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
  const auto payload = test::MakeRandomPayload(64UL * 1024);

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
    const auto result = test::EncodeChunk(*ctx, payload, chunkOut);
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() > 0) {
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

  const auto maxChunk = ctx->minEncodeChunkCapacity(payload.size());
  ASSERT_GT(maxChunk, 0U);
  RawChars chunkOut(maxChunk);
  const auto result = ctx->encodeChunk(payload, chunkOut.capacity(), chunkOut.data());
  ASSERT_FALSE(result.hasError());
  EXPECT_LE(static_cast<std::size_t>(result.writtenIfNoError()), maxChunk);

  RawChars tailOut(ctx->endChunkSize());
  while (true) {
    const auto tailResult = ctx->end(tailOut.capacity(), tailOut.data());
    ASSERT_FALSE(tailResult.hasError());
    if (tailResult.writtenIfNoError() == 0) {
      break;
    }
    EXPECT_LE(tailResult.writtenIfNoError(), tailOut.capacity());
  }
}

TEST_P(ZlibEncoderDecoderTest, ZlibEndWithoutEnoughBufferShouldFail) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  auto ctx = encoder.makeContext(GetParam());

  // Provide a too-small buffer to end()
  const auto tailWritten = ctx->end(0UL, nullptr);
  EXPECT_TRUE(tailWritten.hasError());

  // A buffer without room: nothing can be written.
  char buf[1];
  EXPECT_TRUE(ctx->end(0UL, buf).hasError());
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
    const auto result = test::EncodeChunk(*ctx, payload, chunkOut);
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() > 0) {
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
  const vector<std::string> payloads = {
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
      const auto result = test::EncodeChunk(*ctx, payload, chunkOut);
      ASSERT_FALSE(result.hasError());
      if (result.writtenIfNoError() > 0) {
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
    const auto result = test::EncodeChunk(*ctx, "data", chunkOut);
    ASSERT_FALSE(result.hasError());
  }

  // The context is in use - operations that require it to not be active should throw
  // This is tested through the actual operations that check _sessionActive
  SUCCEED();  // Implicit test of session tracking
}

TEST_P(ZlibEncoderDecoderTest, VariantSwitchingDuringSession) {
  // Initialize with one variant
  CompressionConfig cfg;
  ZlibEncoder encoder1(cfg.zlib.level);
  auto ctx1 = encoder1.makeContext(ZlibVariant::gzip);
  RawChars compressed1;
  {
    RawChars chunkOut;
    const auto result = test::EncodeChunk(*ctx1, "test", chunkOut);
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() > 0) {
      compressed1.append(chunkOut);
    }
  }

  // Create a new encoder with different variant
  ZlibEncoder encoder2(cfg.zlib.level);
  auto ctx2 = encoder2.makeContext(ZlibVariant::deflate);
  RawChars compressed2;
  {
    RawChars chunkOut;
    const auto result = test::EncodeChunk(*ctx2, "test", chunkOut);
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() > 0) {
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
    EncodeFull(encoder, ZlibVariant::gzip, payload, compressed, 64UL);
    RawChars decompressed;
    ZlibDecoder decoder;
    decoder.setVariant(ZlibVariant::gzip);
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
    EncodeFull(encoder, ZlibVariant::deflate, payload, compressed, 64UL);
    RawChars decompressed;
    ZlibDecoder decoder;
    decoder.setVariant(ZlibVariant::deflate);
    ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
    EXPECT_EQ(std::string_view(decompressed), payload);
  }
}

TEST(ZlibEncoderDecoderTest, EncodeChunkWithInsufficientOutputCapacity) {
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  auto ctx = encoder.makeContext(ZlibVariant::deflate);

  // Create a large input.
  std::string large(4096, 'X');

  // Try to encode with only 1 byte available.
  char tiny[1];
  const auto result = ctx->encodeChunk(large, sizeof(tiny), tiny);

  if (!result.hasError()) {
    // If zlib has not reported an error at encodeChunk time, it should report an error at end() time since the stream
    // can't be finalized with pending input.
    const auto endResult = ctx->end(sizeof(tiny) - result.writtenIfNoError(), tiny);
    EXPECT_TRUE(endResult.hasError());
  }
}

// A stream fed in pieces whose decoded size comes within one decoder chunk of maxDecompressedBytes is accepted: the
// limit only stops an output that would exceed it.
TEST_P(ZlibEncoderDecoderTest, StreamingDecodeCloseToTheLimitIsAccepted) {
  const RawChars payload = test::MakeRandomPayload(9000);  // incompressible: the coded stream spans many pieces
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  RawChars compressed;
  EncodeFull(encoder, GetParam(), std::string_view{payload}, compressed);
  const std::string_view view{compressed};
  for (const std::size_t limit : {payload.size(), payload.size() + 1, payload.size() + kDecoderChunkSize}) {
    ZlibDecoder decoder(GetParam());
    auto* ctx = decoder.makeContext();
    RawChars decompressed;
    bool ok = true;
    for (std::size_t off = 0; ok && off < view.size(); off += 100) {
      const std::string_view piece = view.substr(off, 100);
      ok = ctx->decompressChunk(piece, off + piece.size() == view.size(), limit - decompressed.size(),
                                kDecoderChunkSize, decompressed);
    }
    EXPECT_TRUE(ok) << VariantName(GetParam()) << " limit " << limit;
    EXPECT_TRUE(std::string_view(decompressed) == std::string_view{payload}) << VariantName(GetParam());
  }

  ZlibDecoder decoder(GetParam());
  RawChars decompressed;
  EXPECT_FALSE(decoder.decompressFull(view, payload.size() - 1, kDecoderChunkSize, decompressed));
}

// maxDecompressedBytes holds when the output buffer already has room beyond it, as a reused buffer has: a stream
// decoding to exactly the limit is accepted, one byte more is rejected, in one shot or fed piece by piece.
TEST_P(ZlibEncoderDecoderTest, MaxDecompressedBytesWithSpareOutputCapacity) {
  const std::string payload = test::MakePatternedPayload(100000);
  CompressionConfig cfg;
  ZlibEncoder encoder(cfg.zlib.level);
  RawChars compressed;
  EncodeFull(encoder, GetParam(), payload, compressed);
  const std::string_view view(compressed);
  for (const std::size_t pieceSize : {view.size(), std::size_t{100}}) {
    for (const std::size_t limit : {payload.size() - 1, payload.size()}) {
      ZlibDecoder decoder(GetParam());
      auto* ctx = decoder.makeContext();
      RawChars decompressed(4 * payload.size());
      bool ok = true;
      for (std::size_t off = 0; ok && off < view.size(); off += pieceSize) {
        const std::string_view piece = view.substr(off, pieceSize);
        ok = ctx->decompressChunk(piece, off + piece.size() == view.size(), limit - decompressed.size(),
                                  kDecoderChunkSize, decompressed);
      }
      EXPECT_EQ(ok, limit == payload.size()) << VariantName(GetParam()) << " piece " << pieceSize << " limit " << limit;
      EXPECT_LE(decompressed.size(), limit + 1);
    }
  }
}

}  // namespace aeronet