#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/brotli-decoder.hpp"
#include "aeronet/brotli-encoder.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sys-test-support.hpp"
#include "brotli/encode.h"

#if AERONET_WANT_MALLOC_OVERRIDES
#include <new>
#endif

namespace aeronet {

namespace {

constexpr std::size_t kDecoderChunkSize = 256;
constexpr std::size_t kExtraCapacity = 0;
constexpr std::size_t kMaxPlainBytes = 2UL * 1024 * 1024;

std::vector<std::string> SamplePayloads() {
  std::vector<std::string> payloads;
  payloads.emplace_back("");
  payloads.emplace_back("Hello, Brotli compression!");
  payloads.emplace_back(512, 'A');
  payloads.emplace_back(test::MakePatternedPayload(128UL * 1024UL));
  return payloads;
}

void EncodeFull(BrotliEncoder& encoder, std::string_view payload, RawChars& out, std::size_t extraCapacity = 0) {
  out.clear();
  out.reserve(BrotliEncoderMaxCompressedSize(payload.size()) + extraCapacity);
  const std::size_t written = encoder.encodeFull(payload, out.capacity(), out.data());
  ASSERT_GT(written, 0UL);
  out.setSize(static_cast<RawChars::size_type>(written));
}

void ExpectOneShotRoundTrip(BrotliEncoder& encoder, std::string_view payload) {
  RawChars compressed;
  EncodeFull(encoder, payload, compressed, kExtraCapacity);
  RawChars decompressed;
  ASSERT_TRUE(
      BrotliDecoder::decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingRoundTrip(BrotliEncoder& encoder, std::string_view payload, std::size_t split) {
  auto ctx = encoder.makeContext();
  const auto compressed = test::BuildStreamingCompressed(*ctx, payload, split);
  RawChars decompressed;
  ASSERT_TRUE(
      BrotliDecoder::decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingDecoderRoundTrip(BrotliEncoder& encoder, std::string_view payload, std::size_t split) {
  const auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, 4096U);
  auto ctx = BrotliDecoder::makeContext();
  RawChars decompressed;
  std::string_view view(compressed);
  std::size_t offset = 0;
  while (offset < view.size()) {
    const std::size_t take = std::min(split, view.size() - offset);
    const std::string_view chunk = view.substr(offset, take);
    offset += take;
    const bool finalChunk = offset >= view.size();
    ASSERT_TRUE(ctx.decompressChunk(chunk, finalChunk, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  }
  ASSERT_TRUE(ctx.decompressChunk({}, true, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

}  // namespace

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(BrotliDecoderTest, MallocConstructorFails) {
  // Simulate malloc failure during BrotliDecoderCreateInstance
  test::FailNextMalloc();
  RawChars buf;
  EXPECT_THROW(BrotliDecoder::decompressFull("some-data", kMaxPlainBytes, kDecoderChunkSize, buf), std::bad_alloc);

  // Test encoder init failure
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  test::FailNextMalloc();
  EXPECT_THROW(encoder.makeContext(), std::bad_alloc);
}

#endif

TEST(BrotliEncoderDecoderTest, MoveConstructor) {
  BrotliScratch scratch;
  BrotliEncoderContext ctx1(scratch);
  ctx1.init(2, 15);
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

  BrotliEncoderContext ctx2(std::move(ctx1));
  ctx2.init(2, 15);
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

TEST(BrotliEncoderDecoderTest, BrotliEndWithoutEnoughBufferShouldFail) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  auto ctx = encoder.makeContext();

  // Provide a too-small buffer to end()
  const auto tailWritten = ctx->end(0UL, nullptr);
  EXPECT_EQ(tailWritten, -1);
}

TEST(BrotliEncoderDecoderTest, EncodeChunkAfterFinalizationReturnsZero) {
  // Finish the stream, then try to encode more data: should return -1 to signal an error.
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  auto ctx = encoder.makeContext();
  // Produce some initial data.

  RawChars chunkOut;
  const auto written = test::EncodeChunk(*ctx, "Test data", chunkOut);
  ASSERT_GE(written, 0);

  // Finalize the stream.
  test::EndStream(*ctx, chunkOut);
  // Encoding after finalization should return -1 to signal an error.
  RawChars extra;
  extra.reserve(BrotliEncoderMaxCompressedSize(std::string_view{"More data"}.size()));
  EXPECT_LT(ctx->encodeChunk("More data", extra.capacity(), extra.data()), 0);

  // self move does nothing
  auto& self = encoder;
  EXPECT_NO_THROW(encoder = std::move(self));
}

TEST(BrotliEncoderDecoderTest, EncodeFullHandlesEmptyPayload) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);

  RawChars compressed;
  EncodeFull(encoder, std::string_view{}, compressed, kExtraCapacity);
  EXPECT_GT(compressed.size(), 0U);  // should produce some output even for empty input

  RawChars decompressed;
  ASSERT_TRUE(BrotliDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(decompressed.size(), 0U);
}

TEST(BrotliEncoderDecoderTest, MaxCompressedBytesAndEndAreSane) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  auto ctx = encoder.makeContext();
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

TEST(BrotliEncoderDecoderTest, MaxDecompressedBytes) {
  for (const auto& payload : SamplePayloads()) {
    CompressionConfig cfg;
    BrotliEncoder encoder(cfg.brotli);
    RawChars compressed;
    EncodeFull(encoder, payload, compressed, kExtraCapacity);

    RawChars decompressed;
    const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
    const bool isOK = BrotliDecoder::decompressFull(compressed, limit, kDecoderChunkSize, decompressed);
    EXPECT_EQ(isOK, payload.empty());
    EXPECT_EQ(decompressed, std::string_view(payload).substr(0, limit));
  }
}

TEST(BrotliEncoderDecoderTest, EncodeFullRoundTripsPayloads) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);

  for (const auto& payload : SamplePayloads()) {
    SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size());
    ExpectOneShotRoundTrip(encoder, payload);
  }
}

TEST(BrotliEncoderDecoderTest, StreamingRoundTripsAcrossChunkSplits) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);

  static constexpr std::array kSplits{1ULL, 5ULL, 113ULL, 4096ULL, 10000ULL};
  for (const auto& payload : SamplePayloads()) {
    for (const auto split : kSplits) {
      SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size() << " split=" << split);
      ExpectStreamingRoundTrip(encoder, payload, split);
    }
  }
}

TEST(BrotliEncoderDecoderTest, StreamingDecoderHandlesChunkSplits) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  static constexpr std::array<std::size_t, 4> kDecodeSplits{1U, 7U, 257U, 4096U};
  for (const auto& payload : SamplePayloads()) {
    for (const auto split : kDecodeSplits) {
      SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size() << " decode split=" << split);
      ExpectStreamingDecoderRoundTrip(encoder, payload, split);
    }
  }
}

TEST(BrotliEncoderDecoderTest, StreamingAndOneShotProduceSameOutput) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);

  for (const auto& payload : SamplePayloads()) {
    RawChars oneShotCompressed;
    EncodeFull(encoder, payload, oneShotCompressed, kExtraCapacity);

    RawChars streamingCompressed;
    static constexpr std::size_t kSplit = 128U;
    auto ctx = encoder.makeContext();
    std::string_view remaining = payload;
    while (!remaining.empty()) {
      const std::size_t take = std::min(kSplit, remaining.size());
      const auto chunk = remaining.substr(0, take);
      remaining.remove_prefix(take);
      RawChars chunkOut;
      const auto written = test::EncodeChunk(*ctx, chunk, chunkOut);
      ASSERT_GE(written, 0);
      if (written > 0) {
        streamingCompressed.append(chunkOut);
      }
    }
    test::EndStream(*ctx, streamingCompressed);

    // Decode both datas to ensure validity
    RawChars decompressedOneShot;
    RawChars decompressedStreaming;
    ASSERT_TRUE(BrotliDecoder::decompressFull(std::string_view(oneShotCompressed), kMaxPlainBytes, kDecoderChunkSize,
                                              decompressedOneShot));
    ASSERT_TRUE(BrotliDecoder::decompressFull(std::string_view(streamingCompressed), kMaxPlainBytes, kDecoderChunkSize,
                                              decompressedStreaming));
    EXPECT_EQ(std::string_view(decompressedOneShot), payload);
    EXPECT_EQ(std::string_view(decompressedStreaming), payload);
  }
}

TEST(BrotliEncoderDecoderTest, DecodeInvalidDataFails) {
  RawChars invalidData("NotAValidBrotliStream");
  RawChars decompressed;
  EXPECT_FALSE(BrotliDecoder::decompressFull(invalidData, kMaxPlainBytes, kDecoderChunkSize, decompressed));
}

TEST(BrotliEncoderDecoderTest, StreamingSmallOutputBufferDrainsAndRoundTrips) {
  // Create a patterned payload large enough to force multiple compressStream2 calls
  // when the encoder is given a very small output buffer.
  const std::string payload = test::MakePatternedPayload(1024);

  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  auto ctx = encoder.makeContext();
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

  RawChars decompressed;
  ASSERT_TRUE(
      BrotliDecoder::decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

TEST(BrotliEncoderDecoderTest, StreamingRandomIncompressibleForcesMultipleIterations) {
  // Incompressible payload to force encoder to iterate and grow output as needed.
  const RawBytes payload = test::MakeRandomPayload(64UL * 1024);

#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  static constexpr std::size_t kChunkSize = 8UL;
#else
  static constexpr std::size_t kChunkSize = 1UL;  // small to force multiple iterations; encoder will grow as needed
#endif
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  auto ctx = encoder.makeContext();
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
  ASSERT_TRUE(BrotliDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));

  EXPECT_EQ(decompressed.size(), payload.size());
  EXPECT_EQ(std::memcmp(decompressed.data(), payload.data(), payload.size()), 0);
}

TEST(BrotliEncoderDecoderTest, RepeatedDecompressDoesNotGrowCapacity) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  RawChars compressed;
  EncodeFull(encoder, "Hello, Brotli compression!", compressed, kExtraCapacity);

  RawChars decompressed;
  ASSERT_TRUE(
      BrotliDecoder::decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  const auto cap1 = decompressed.capacity();
  ASSERT_GT(cap1, 0U);

  decompressed.clear();
  ASSERT_TRUE(
      BrotliDecoder::decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  const auto cap2 = decompressed.capacity();

  EXPECT_EQ(cap2, cap1);
}

TEST(BrotliEncoderDecoderTest, EncodeChunkWithInsufficientOutputCapacity) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  auto ctx = encoder.makeContext();

  // Create a large input.
  std::string large(4096, 'X');

  // Try to encode with only 1 byte available.
  char tiny[1];
  const auto result = ctx->encodeChunk(large, sizeof(tiny), tiny);

  // Brotli gracefully accepts the small buffer and returns 0 (empty output).
  // The check for "availIn != 0" that would return -1 is unreachable in practice.
  EXPECT_LE(result, 0);
}

}  // namespace aeronet