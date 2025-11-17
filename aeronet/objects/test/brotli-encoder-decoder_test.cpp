#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/brotli-decoder.hpp"
#include "aeronet/brotli-encoder.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/raw-chars.hpp"

namespace {

constexpr std::size_t kEncoderChunkSize = 1024;
constexpr std::size_t kDecoderChunkSize = 256;
constexpr std::size_t kExtraCapacity = 0;
constexpr std::size_t kMaxPlainBytes = 2UL * 1024 * 1024;

std::string makePatternedPayload(std::size_t size) {
  std::string payload;
  payload.reserve(size);
  for (std::size_t pos = 0; pos < size; ++pos) {
    payload.push_back(static_cast<char>('a' + static_cast<int>(pos % 23U)));
  }
  return payload;
}

std::vector<std::string> samplePayloads() {
  std::vector<std::string> payloads;
  payloads.emplace_back("");
  payloads.emplace_back("Hello, Brotli compression!");
  payloads.emplace_back(512, 'A');
  payloads.emplace_back(makePatternedPayload(128UL * 1024UL));
  return payloads;
}

void ExpectOneShotRoundTrip(aeronet::BrotliEncoder& encoder, std::string_view payload) {
  aeronet::RawChars compressed;
  encoder.encodeFull(kExtraCapacity, payload, compressed);
  aeronet::RawChars decompressed;
  ASSERT_TRUE(aeronet::BrotliDecoder::Decompress(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize,
                                                 decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingRoundTrip(aeronet::BrotliEncoder& encoder, std::string_view payload, std::size_t split) {
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

  aeronet::RawChars decompressed;
  ASSERT_TRUE(aeronet::BrotliDecoder::Decompress(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize,
                                                 decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

}  // namespace

TEST(BrotliEncoderDecoderTest, EncodeFullRoundTripsPayloads) {
  aeronet::CompressionConfig cfg;
  aeronet::BrotliEncoder encoder(cfg);

  for (const auto& payload : samplePayloads()) {
    SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size());
    ExpectOneShotRoundTrip(encoder, payload);
  }
}

TEST(BrotliEncoderDecoderTest, StreamingRoundTripsAcrossChunkSplits) {
  aeronet::CompressionConfig cfg;
  aeronet::BrotliEncoder encoder(cfg);

  constexpr std::array<std::size_t, 4> kSplits{1U, 5U, 113U, 4096U};
  for (const auto& payload : samplePayloads()) {
    for (const auto split : kSplits) {
      SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size() << " split=" << split);
      ExpectStreamingRoundTrip(encoder, payload, split);
    }
  }
}
