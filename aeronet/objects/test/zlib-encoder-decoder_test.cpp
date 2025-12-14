#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-encoder.hpp"
#include "aeronet/zlib-stream-raii.hpp"

using namespace aeronet;

namespace {

constexpr std::size_t kEncoderChunkSize = 1536;
constexpr std::size_t kDecoderChunkSize = 512;
constexpr std::size_t kExtraCapacity = 0;
constexpr std::size_t kMaxPlainBytes = 2UL * 1024 * 1024;

std::string makePatternedPayload(std::size_t size) {
  std::string payload;
  payload.reserve(size);
  for (std::size_t pos = 0; pos < size; ++pos) {
    payload.push_back(static_cast<char>('a' + static_cast<int>(pos % 13U)));
  }
  return payload;
}

std::vector<std::string> samplePayloads() {
  std::vector<std::string> payloads;
  payloads.emplace_back("");
  payloads.emplace_back("gzip -> deflate parity test");
  payloads.emplace_back(2048, 'x');
  payloads.emplace_back(makePatternedPayload(64UL * 1024UL));
  return payloads;
}

const char* variantName(ZStreamRAII::Variant variant) {
  return variant == ZStreamRAII::Variant::gzip ? "gzip" : "deflate";
}

void ExpectOneShotRoundTrip(ZStreamRAII::Variant variant, std::string_view payload) {
  CompressionConfig cfg;
  ZlibEncoder encoder(variant, cfg);
  RawChars compressed;
  encoder.encodeFull(kExtraCapacity, payload, compressed);

  const bool isGzip = variant == ZStreamRAII::Variant::gzip;
  RawChars decompressed;
  ASSERT_TRUE(
      ZlibDecoder::Decompress(std::string_view(compressed), isGzip, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingRoundTrip(ZStreamRAII::Variant variant, std::string_view payload, std::size_t split) {
  CompressionConfig cfg;
  ZlibEncoder encoder(variant, cfg);
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

  const bool isGzip = variant == ZStreamRAII::Variant::gzip;
  RawChars decompressed;
  ASSERT_TRUE(
      ZlibDecoder::Decompress(std::string_view(compressed), isGzip, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

RawChars BuildStreamingCompressed(ZStreamRAII::Variant variant, std::string_view payload) {
  CompressionConfig cfg;
  ZlibEncoder encoder(variant, cfg);
  RawChars compressed;
  auto ctx = encoder.makeContext();
  std::string_view remaining = payload;
  while (!remaining.empty()) {
    const std::size_t take = std::min<std::size_t>(remaining.size(), 4096U);
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

void ExpectStreamingDecoderRoundTrip(ZStreamRAII::Variant variant, std::string_view payload, std::size_t split) {
  const auto compressed = BuildStreamingCompressed(variant, payload);
  ZlibDecoder decoder(variant == ZStreamRAII::Variant::gzip);
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
  for (const auto& payload : samplePayloads()) {
    SCOPED_TRACE(testing::Message() << variantName(variant) << " payload bytes=" << payload.size());
    ExpectOneShotRoundTrip(variant, payload);
  }
}

TEST_P(ZlibEncoderDecoderTest, MaxDecompressedBytes) {
  const auto variant = GetParam();
  for (const auto& payload : samplePayloads()) {
    SCOPED_TRACE(testing::Message() << variantName(variant) << " payload bytes=" << payload.size());
    CompressionConfig cfg;
    ZlibEncoder encoder(variant, cfg);
    RawChars compressed;
    encoder.encodeFull(kExtraCapacity, payload, compressed);

    const bool isGzip = variant == ZStreamRAII::Variant::gzip;
    RawChars decompressed;
    const std::size_t limit = payload.size() > 0 ? payload.size() - 1 : 0;
    const bool isOK = ZlibDecoder::Decompress(compressed, isGzip, limit, kDecoderChunkSize, decompressed);
    EXPECT_EQ(isOK, payload.empty());
    EXPECT_EQ(decompressed, std::string_view(payload).substr(0, limit));
  }
}

TEST_P(ZlibEncoderDecoderTest, EmptyChunksShouldAlwaysSucceed) {
  const auto variant = GetParam();
  ZlibDecoder decoder(variant == ZStreamRAII::Variant::gzip);
  auto ctx = decoder.makeContext();
  ASSERT_TRUE(ctx);
  RawChars decompressed;
  EXPECT_TRUE(ctx->decompressChunk({}, false, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_TRUE(ctx->decompressChunk({}, true, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_TRUE(decompressed.empty());
}

TEST_P(ZlibEncoderDecoderTest, InflateErrorOnInvalidData) {
  const auto variant = GetParam();
  RawChars invalidData("NotAValidZlibStream");
  RawChars decompressed;
  const bool isGzip = variant == ZStreamRAII::Variant::gzip;
  EXPECT_FALSE(
      ZlibDecoder::Decompress(std::string_view(invalidData), isGzip, kMaxPlainBytes, kDecoderChunkSize, decompressed));
}

TEST_P(ZlibEncoderDecoderTest, StreamingRoundTripsAcrossChunkSplits) {
  const auto variant = GetParam();
  static constexpr std::array kSplits{1ULL, 9ULL, 257ULL, 4096ULL, 10000ULL};
  for (const auto& payload : samplePayloads()) {
    for (const auto split : kSplits) {
      SCOPED_TRACE(testing::Message() << variantName(variant) << " payload bytes=" << payload.size()
                                      << " split=" << split);
      ExpectStreamingRoundTrip(variant, payload, split);
    }
  }
}

TEST_P(ZlibEncoderDecoderTest, StreamingDecoderHandlesChunkSplits) {
  const auto variant = GetParam();
  static constexpr std::array<std::size_t, 4> kDecodeSplits{1U, 7U, 257U, 4096U};
  for (const auto& payload : samplePayloads()) {
    for (const auto split : kDecodeSplits) {
      SCOPED_TRACE(testing::Message() << variantName(variant) << " payload bytes=" << payload.size()
                                      << " decode split=" << split);
      ExpectStreamingDecoderRoundTrip(variant, payload, split);
    }
  }
}
