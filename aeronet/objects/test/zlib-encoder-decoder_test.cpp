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

const char* variantName(aeronet::details::ZStreamRAII::Variant variant) {
  return variant == aeronet::details::ZStreamRAII::Variant::gzip ? "gzip" : "deflate";
}

void ExpectOneShotRoundTrip(aeronet::details::ZStreamRAII::Variant variant, std::string_view payload) {
  aeronet::CompressionConfig cfg;
  aeronet::ZlibEncoder encoder(variant, cfg);
  aeronet::RawChars compressed;
  encoder.encodeFull(kExtraCapacity, payload, compressed);

  const bool isGzip = variant == aeronet::details::ZStreamRAII::Variant::gzip;
  aeronet::RawChars decompressed;
  ASSERT_TRUE(aeronet::ZlibDecoder::Decompress(std::string_view(compressed), isGzip, kMaxPlainBytes, kDecoderChunkSize,
                                               decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingRoundTrip(aeronet::details::ZStreamRAII::Variant variant, std::string_view payload,
                              std::size_t split) {
  aeronet::CompressionConfig cfg;
  aeronet::ZlibEncoder encoder(variant, cfg);
  aeronet::RawChars compressed;
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

  const bool isGzip = variant == aeronet::details::ZStreamRAII::Variant::gzip;
  aeronet::RawChars decompressed;
  ASSERT_TRUE(aeronet::ZlibDecoder::Decompress(std::string_view(compressed), isGzip, kMaxPlainBytes, kDecoderChunkSize,
                                               decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

}  // namespace

class ZlibEncoderDecoderTest : public ::testing::TestWithParam<aeronet::details::ZStreamRAII::Variant> {};

INSTANTIATE_TEST_SUITE_P(Variants, ZlibEncoderDecoderTest,
                         ::testing::Values(aeronet::details::ZStreamRAII::Variant::gzip,
                                           aeronet::details::ZStreamRAII::Variant::deflate));

TEST_P(ZlibEncoderDecoderTest, EncodeFullRoundTripsPayloads) {
  const auto variant = GetParam();
  for (const auto& payload : samplePayloads()) {
    SCOPED_TRACE(testing::Message() << variantName(variant) << " payload bytes=" << payload.size());
    ExpectOneShotRoundTrip(variant, payload);
  }
}

TEST_P(ZlibEncoderDecoderTest, StreamingRoundTripsAcrossChunkSplits) {
  const auto variant = GetParam();
  constexpr std::array<std::size_t, 4> kSplits{1U, 9U, 257U, 4096U};
  for (const auto& payload : samplePayloads()) {
    for (const auto split : kSplits) {
      SCOPED_TRACE(testing::Message() << variantName(variant) << " payload bytes=" << payload.size()
                                      << " split=" << split);
      ExpectStreamingRoundTrip(variant, payload, split);
    }
  }
}
