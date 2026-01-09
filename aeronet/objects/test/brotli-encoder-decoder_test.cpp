#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/brotli-decoder.hpp"
#include "aeronet/brotli-encoder.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sys-test-support.hpp"

#if AERONET_WANT_MALLOC_OVERRIDES
#include <new>
#endif

namespace aeronet {

namespace {

constexpr std::size_t kEncoderChunkSize = 512;
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

void ExpectOneShotRoundTrip(BrotliEncoder& encoder, std::string_view payload) {
  RawChars compressed;
  encoder.encodeFull(kExtraCapacity, payload, compressed);
  RawChars decompressed;
  ASSERT_TRUE(
      BrotliDecoder::decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

RawChars BuildStreamingCompressed(BrotliEncoder& encoder, std::string_view payload, std::size_t split) {
  RawChars compressed;
  auto ctx = encoder.makeContext();
  std::string_view remaining = payload;
  while (!remaining.empty()) {
    const std::size_t take = std::min(split, remaining.size());
    const auto chunk = remaining.substr(0, take);
    remaining.remove_prefix(take);
    const auto produced = ctx->encodeChunk(kEncoderChunkSize, chunk);
    if (!produced.empty()) {
      compressed.append(produced);
    }
  }
  const auto tail = ctx->encodeChunk(kEncoderChunkSize, {});
  if (!tail.empty()) {
    compressed.append(tail);
  }
  return compressed;
}

void ExpectStreamingRoundTrip(BrotliEncoder& encoder, std::string_view payload, std::size_t split) {
  const auto compressed = BuildStreamingCompressed(encoder, payload, split);
  RawChars decompressed;
  ASSERT_TRUE(
      BrotliDecoder::decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingDecoderRoundTrip(BrotliEncoder& encoder, std::string_view payload, std::size_t split) {
  const auto compressed = BuildStreamingCompressed(encoder, payload, 4096U);
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

  test::FailNextMalloc();
  EXPECT_THROW(BrotliEncoderContext(buf, 5, 22), std::bad_alloc);
}

#endif

TEST(BrotliEncoderDecoderTest, EncodeFullHandlesEmptyPayload) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg);

  RawChars compressed;
  encoder.encodeFull(kExtraCapacity, std::string_view{}, compressed);
  EXPECT_GT(compressed.size(), 0U);  // should produce some output even for empty input

  RawChars decompressed;
  ASSERT_TRUE(BrotliDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(decompressed.size(), 0U);
}

TEST(BrotliEncoderDecoderTest, MaxDecompressedBytes) {
  for (const auto& payload : SamplePayloads()) {
    CompressionConfig cfg;
    BrotliEncoder encoder(cfg);
    RawChars compressed;
    encoder.encodeFull(kExtraCapacity, payload, compressed);

    RawChars decompressed;
    const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
    const bool isOK = BrotliDecoder::decompressFull(compressed, limit, kDecoderChunkSize, decompressed);
    EXPECT_EQ(isOK, payload.empty());
    EXPECT_EQ(decompressed, std::string_view(payload).substr(0, limit));
  }
}

TEST(BrotliEncoderDecoderTest, EncodeFullRoundTripsPayloads) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg);

  for (const auto& payload : SamplePayloads()) {
    SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size());
    ExpectOneShotRoundTrip(encoder, payload);
  }
}

TEST(BrotliEncoderDecoderTest, StreamingRoundTripsAcrossChunkSplits) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg);

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
  BrotliEncoder encoder(cfg);
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
  BrotliEncoder encoder(cfg);

  for (const auto& payload : SamplePayloads()) {
    RawChars oneShotCompressed;
    encoder.encodeFull(kExtraCapacity, payload, oneShotCompressed);

    RawChars streamingCompressed;
    static constexpr std::size_t kSplit = 128U;
    auto ctx = encoder.makeContext();
    std::string_view remaining = payload;
    while (!remaining.empty()) {
      const std::size_t take = std::min(kSplit, remaining.size());
      const auto chunk = remaining.substr(0, take);
      remaining.remove_prefix(take);
      const auto produced = ctx->encodeChunk(kEncoderChunkSize, chunk);
      if (!produced.empty()) {
        streamingCompressed.append(produced);
      }
    }
    const auto tail = ctx->encodeChunk(kEncoderChunkSize, {});
    if (!tail.empty()) {
      streamingCompressed.append(tail);
    }

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
  BrotliEncoder encoder(cfg, 1);
  auto ctx = encoder.makeContext();
  RawChars compressed;
  const auto produced = ctx->encodeChunk(1, std::string_view(payload));
  compressed.append(produced);
  const auto tail = ctx->encodeChunk(1, {});
  compressed.append(tail);

  RawChars decompressed;
  ASSERT_TRUE(
      BrotliDecoder::decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

TEST(BrotliEncoderDecoderTest, StreamingRandomIncompressibleForcesMultipleIterations) {
  // Incompressible payload to force encoder to iterate and grow output as needed.
  const RawBytes payload = test::MakeRandomPayload(256UL * 1024);

#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  static constexpr std::size_t kChunkSize = 8UL;
#else
  static constexpr std::size_t kChunkSize = 1UL;  // small to force multiple iterations; encoder will grow as needed
#endif
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg, kChunkSize);
  auto ctx = encoder.makeContext();
  RawChars compressed;

  const auto produced =
      ctx->encodeChunk(kChunkSize, std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
  compressed.append(produced);
  const auto tail = ctx->encodeChunk(kChunkSize, {});
  compressed.append(tail);

  // Expect more than one chunk worth of output, implying multiple loop iterations.
  ASSERT_GT(compressed.size(), kChunkSize);

  RawChars decompressed;
  ASSERT_TRUE(BrotliDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));

  EXPECT_EQ(decompressed.size(), payload.size());
  EXPECT_EQ(std::memcmp(decompressed.data(), payload.data(), payload.size()), 0);
}

TEST(BrotliEncoderDecoderTest, RepeatedDecompressDoesNotGrowCapacity) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg);
  RawChars compressed;
  encoder.encodeFull(kExtraCapacity, "Hello, Brotli compression!", compressed);

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

}  // namespace aeronet