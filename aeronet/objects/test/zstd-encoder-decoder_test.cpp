#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/zstd-decoder.hpp"
#include "aeronet/zstd-encoder.hpp"

#if AERONET_WANT_MALLOC_OVERRIDES
#include <new>
#endif

namespace aeronet {

namespace {

constexpr std::size_t kEncoderChunkSize = 2048;
constexpr std::size_t kChunkSizes[] = {1, 8, 64, 128, 512, 2048, 8192};
constexpr std::size_t kDecoderChunkSize = 512;
constexpr std::size_t kExtraCapacity = 0;
constexpr std::size_t kMaxPlainBytes = 4UL * 1024 * 1024;

std::vector<std::string> SamplePayloads() {
  std::vector<std::string> payloads;
  payloads.reserve(4);
  payloads.emplace_back("");
  payloads.emplace_back("Zstd keeps strings sharp.");
  payloads.emplace_back(4096, 'Z');
  payloads.emplace_back(test::MakePatternedPayload(256UL * 1024UL));
  return payloads;
}

void ExpectOneShotRoundTrip(std::string_view payload) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg);
  RawChars compressed;
  encoder.encodeFull(kExtraCapacity, payload, compressed);

  RawChars decompressed;
  ASSERT_TRUE(ZstdDecoder::Decompress(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

RawChars BuildStreamingCompressed(std::string_view payload, std::size_t split,
                                  std::size_t chunkSize = kEncoderChunkSize) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg);
  RawChars compressed;
  auto ctx = encoder.makeContext();
  std::string_view remaining = payload;
  while (!remaining.empty()) {
    const std::size_t take = std::min(split, remaining.size());
    const auto chunk = remaining.substr(0, take);
    remaining.remove_prefix(take);
    const auto produced = ctx->encodeChunk(chunkSize, chunk);
    compressed.append(produced);
  }
  const auto tail = ctx->encodeChunk(chunkSize, {});
  compressed.append(tail);
  return compressed;
}

void ExpectStreamingRoundTrip(std::string_view payload, std::size_t split, std::size_t chunkSize = kEncoderChunkSize) {
  const auto compressed = BuildStreamingCompressed(payload, split, chunkSize);
  RawChars decompressed;
  ASSERT_TRUE(ZstdDecoder::Decompress(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingDecoderRoundTrip(std::string_view payload, std::size_t split,
                                     std::size_t chunkSize = kEncoderChunkSize) {
  const auto compressed = BuildStreamingCompressed(payload, 4096U, chunkSize);
  ZstdDecoder decoder;
  auto ctx = decoder.makeContext();
  ASSERT_TRUE(ctx);
  RawChars decompressed;
  std::string_view view(compressed);
  std::size_t offset = 0;
  while (offset < view.size()) {
    const std::size_t take = std::min(split, view.size() - offset);
    const std::string_view chunk = view.substr(offset, take);
    offset += take;
    const bool finalChunk = offset >= view.size();
    ASSERT_TRUE(ctx->decompressChunk(chunk, finalChunk, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  }
  ASSERT_TRUE(ctx->decompressChunk({}, true, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

}  // namespace

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(ZstdEncoderDecoderTest, MallocConstructorFails) {
  // Simulate malloc failure during BrotliDecoderCreateInstance
  auto compressed = BuildStreamingCompressed("some-data", 4096U);
  test::FailNextMalloc();
  RawChars buf;
  EXPECT_THROW(ZstdDecoder::Decompress(compressed, kMaxPlainBytes, kDecoderChunkSize, buf), std::bad_alloc);
}

TEST(ZstdEncoderDecoderTest, ZstdContext) {
  // Simulate malloc failure during BrotliDecoderCreateInstance
  auto compressed = BuildStreamingCompressed("some-data", 4096U);
  test::FailNextMalloc();
  EXPECT_THROW(details::ZstdContextRAII ctx(3, 0), std::bad_alloc);
}

TEST(ZstdEncoderDecoderTest, EncodeFails) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg);
  RawChars buf;
  test::FailNextMalloc();
  EXPECT_THROW(encoder.encodeFull(0, "some-data", buf), std::runtime_error);

  auto ctx = encoder.makeContext();
  test::FailNextMalloc();
  EXPECT_THROW(ctx->encodeChunk(0, "some-data"), std::runtime_error);
}

#endif

TEST(ZstdEncoderDecoderTest, EncodeFullRoundTripsPayloads) {
  for (const auto& payload : SamplePayloads()) {
    SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size());
    ExpectOneShotRoundTrip(payload);
  }
}

TEST(ZstdEncoderDecoderTest, MaxDecompressedBytesFull) {
  for (const auto& payload : SamplePayloads()) {
    CompressionConfig cfg;
    ZstdEncoder encoder(cfg);
    RawChars compressed;
    encoder.encodeFull(kExtraCapacity, payload, compressed);

    RawChars decompressed;
    const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
    EXPECT_EQ(ZstdDecoder::Decompress(compressed, limit, kDecoderChunkSize, decompressed), payload.empty());
  }
}

TEST(ZstdEncoderDecoderTest, MaxDecompressedBytesStreaming) {
  for (std::size_t chunkSize : kChunkSizes) {
    for (const auto& payload : SamplePayloads()) {
      const auto compressed = BuildStreamingCompressed(payload, 8U, chunkSize);
      RawChars decompressed;
      const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
      EXPECT_EQ(ZstdDecoder::Decompress(compressed, limit, kDecoderChunkSize, decompressed), payload.empty());
    }
  }
}

TEST(ZstdEncoderDecoderTest, StreamingRoundTripsAcrossChunkSplits) {
  static constexpr std::array kSplits{1ULL, 7ULL, 257ULL, 8192ULL, 10000ULL};
  for (const auto& payload : SamplePayloads()) {
    for (const auto split : kSplits) {
      SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size() << " split=" << split);
      ExpectStreamingRoundTrip(payload, split);
    }
  }
}

TEST(ZstdEncoderDecoderTest, StreamingDecoderHandlesChunkSplits) {
  static constexpr std::array<std::size_t, 4> kDecodeSplits{1U, 7U, 257U, 4096U};
  for (const auto& payload : SamplePayloads()) {
    for (const auto split : kDecodeSplits) {
      SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size() << " decode split=" << split);
      ExpectStreamingDecoderRoundTrip(payload, split);
    }
  }
}

TEST(ZstdEncoderDecoderTest, DecodeInvalidDataFailsFullContentSizeError) {
  RawChars invalidData("NotValidZstdData");
  RawChars decompressed;
  EXPECT_FALSE(ZstdDecoder::Decompress(invalidData, kMaxPlainBytes, kDecoderChunkSize, decompressed));
}

TEST(ZstdEncoderDecoderTest, DecodeInvalidDataFailsFull) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg);
  RawChars compressed;
  encoder.encodeFull(kExtraCapacity, std::string(512, 'A'), compressed);

  // Corrupt the compressed data
  ASSERT_GT(compressed.size(), 13U);
  ++compressed[13];

  RawChars decompressed;
  EXPECT_FALSE(ZstdDecoder::Decompress(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
}

TEST(ZstdEncoderDecoderTest, DecodeInvalidDataFailsStreaming) {
  auto compressed = BuildStreamingCompressed("some-data", 4096U);
  ASSERT_GT(compressed.size(), 4U);
  ++compressed[4];  // Corrupt the data
  RawChars decompressed;
  EXPECT_FALSE(ZstdDecoder::Decompress(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_NE(std::string_view(decompressed), "some-data");
}

}  // namespace aeronet