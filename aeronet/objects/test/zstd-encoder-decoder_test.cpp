#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/compression-config.hpp"
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
constexpr std::size_t kDecoderChunkSize = 512;
constexpr std::size_t kExtraCapacity = 0;
constexpr std::size_t kMaxPlainBytes = 4UL * 1024 * 1024;

std::string makePatternedPayload(std::size_t size) {
  std::string payload;
  payload.reserve(size);
  for (std::size_t pos = 0; pos < size; ++pos) {
    payload.push_back(static_cast<char>('A' + static_cast<int>(pos % 17U)));
  }
  return payload;
}

std::vector<std::string> samplePayloads() {
  std::vector<std::string> payloads;
  payloads.emplace_back("");
  payloads.emplace_back("Zstd keeps strings sharp.");
  payloads.emplace_back(4096, 'Z');
  payloads.emplace_back(makePatternedPayload(256UL * 1024UL));
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

RawChars BuildStreamingCompressed(std::string_view payload, std::size_t split) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg);
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

void ExpectStreamingRoundTrip(std::string_view payload, std::size_t split) {
  const auto compressed = BuildStreamingCompressed(payload, split);
  RawChars decompressed;
  ASSERT_TRUE(ZstdDecoder::Decompress(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingDecoderRoundTrip(std::string_view payload, std::size_t split) {
  const auto compressed = BuildStreamingCompressed(payload, 4096U);
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

#endif

TEST(ZstdEncoderDecoderTest, EncodeFullRoundTripsPayloads) {
  for (const auto& payload : samplePayloads()) {
    SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size());
    ExpectOneShotRoundTrip(payload);
  }
}

TEST(ZstdEncoderDecoderTest, MaxDecompressedBytesFull) {
  for (const auto& payload : samplePayloads()) {
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
  for (const auto& payload : samplePayloads()) {
    const auto compressed = BuildStreamingCompressed(payload, 8U);
    RawChars decompressed;
    const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
    EXPECT_EQ(ZstdDecoder::Decompress(compressed, limit, kDecoderChunkSize, decompressed), payload.empty());
  }
}

TEST(ZstdEncoderDecoderTest, StreamingRoundTripsAcrossChunkSplits) {
  static constexpr std::array kSplits{1ULL, 7ULL, 257ULL, 8192ULL, 10000ULL};
  for (const auto& payload : samplePayloads()) {
    for (const auto split : kSplits) {
      SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size() << " split=" << split);
      ExpectStreamingRoundTrip(payload, split);
    }
  }
}

TEST(ZstdEncoderDecoderTest, StreamingDecoderHandlesChunkSplits) {
  static constexpr std::array<std::size_t, 4> kDecodeSplits{1U, 7U, 257U, 4096U};
  for (const auto& payload : samplePayloads()) {
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