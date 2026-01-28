#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/zstd-decoder.hpp"
#include "aeronet/zstd-encoder.hpp"

#if AERONET_WANT_MALLOC_OVERRIDES
#include <new>
#include <stdexcept>
#endif

namespace aeronet {

namespace {

RawChars buf;

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

void EncodeFull(ZstdEncoder& encoder, std::string_view payload, RawChars& out, std::size_t extraCapacity = 0) {
  out.clear();
  out.reserve(ZSTD_compressBound(payload.size()) + extraCapacity);
  const std::size_t written = encoder.encodeFull(payload, out.capacity(), out.data());
  ASSERT_GT(written, 0UL);
  out.setSize(static_cast<RawChars::size_type>(written));
}

void ExpectOneShotRoundTrip(std::string_view payload) {
  CompressionConfig cfg;
  ZstdEncoder encoder(buf, cfg.zstd);
  RawChars compressed;
  EncodeFull(encoder, payload, compressed, kExtraCapacity);

  RawChars decompressed;
  ASSERT_TRUE(
      ZstdDecoder::decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

RawChars BuildStreamingCompressed(std::string_view payload, std::size_t split) {
  CompressionConfig cfg;
  ZstdEncoder encoder(buf, cfg.zstd);
  RawChars compressed;
  auto ctx = encoder.makeContext();
  std::string_view remaining = payload;
  while (!remaining.empty()) {
    const std::size_t take = std::min(split, remaining.size());
    const auto chunk = remaining.substr(0, take);
    remaining.remove_prefix(take);
    const auto produced = ctx->encodeChunk(chunk);
    compressed.append(produced);
  }
  const auto tail = ctx->encodeChunk({});
  compressed.append(tail);
  return compressed;
}

void ExpectStreamingRoundTrip(std::string_view payload, std::size_t split) {
  const auto compressed = BuildStreamingCompressed(payload, split);
  RawChars decompressed;
  ASSERT_TRUE(ZstdDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingDecoderRoundTrip(std::string_view payload, std::size_t split) {
  const auto compressed = BuildStreamingCompressed(payload, 4096U);
  auto ctx = ZstdDecoder::makeContext();
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

TEST(ZstdEncoderDecoderTest, MallocConstructorFails) {
  auto compressed = BuildStreamingCompressed("some-data", 4096U);
  test::FailNextMalloc();
  RawChars buf;
  EXPECT_THROW(ZstdDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, buf), std::bad_alloc);
}

TEST(ZstdEncoderDecoderTest, ZstdContextInitFails) {
  CompressionConfig cfg;
  ZstdEncoder encoder(buf, cfg.zstd);
  test::FailNextMalloc();
  EXPECT_THROW(encoder.makeContext(), std::bad_alloc);
}

TEST(ZstdEncoderDecoderTest, EncodeFails) {
  CompressionConfig cfg;
  ZstdEncoder encoder(buf, cfg.zstd);
  RawChars buf(std::string_view{"some-data"}.size());
  EXPECT_EQ(encoder.encodeFull("some-data", 0UL, buf.data()), 0UL);

  auto ctx = encoder.makeContext();
  test::FailNextMalloc();
  EXPECT_THROW(ctx->encodeChunk("some-data"), std::runtime_error);
}

#endif

TEST(ZstdEncoderContext, MoveConstructor) {
  ZstdEncoderContext ctx1(buf);
  ctx1.init(2, 15);
  std::string produced;
  produced.append(ctx1.encodeChunk("some-data"));
  produced.append(ctx1.encodeChunk({}));

  EXPECT_GT(produced.size(), 0UL);

  ZstdEncoderContext ctx2(std::move(ctx1));
  ctx2.init(2, 15);
  produced.assign(ctx2.encodeChunk("more-data"));
  produced.append(ctx2.encodeChunk({}));

  EXPECT_GT(produced.size(), 0UL);

  // self move does nothing
  auto& self = ctx2;
  ctx2 = std::move(self);
}

TEST(ZstdEncoderDecoderTest, EncodeFullRoundTripsPayloads) {
  for (const auto& payload : SamplePayloads()) {
    SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size());
    ExpectOneShotRoundTrip(payload);
  }
}

TEST(ZstdEncoderDecoderTest, MaxDecompressedBytesFull) {
  for (const auto& payload : SamplePayloads()) {
    CompressionConfig cfg;
    ZstdEncoder encoder(buf, cfg.zstd);
    RawChars compressed;
    EncodeFull(encoder, payload, compressed, kExtraCapacity);

    RawChars decompressed;
    const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
    EXPECT_EQ(ZstdDecoder::decompressFull(compressed, limit, kDecoderChunkSize, decompressed), payload.empty());
  }
}

TEST(ZstdEncoderDecoderTest, MaxDecompressedBytesStreaming) {
  for (const auto& payload : SamplePayloads()) {
    const auto compressed = BuildStreamingCompressed(payload, 8U);
    RawChars decompressed;
    const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
    EXPECT_EQ(ZstdDecoder::decompressFull(compressed, limit, kDecoderChunkSize, decompressed), payload.empty());
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
  EXPECT_FALSE(ZstdDecoder::decompressFull(invalidData, kMaxPlainBytes, kDecoderChunkSize, decompressed));
}

TEST(ZstdEncoderDecoderTest, DecodeInvalidDataFailsFull) {
  CompressionConfig cfg;
  ZstdEncoder encoder(buf, cfg.zstd);
  RawChars compressed;
  EncodeFull(encoder, std::string(512, 'A'), compressed, kExtraCapacity);

  // Corrupt the compressed data
  ASSERT_GT(compressed.size(), 13U);
  ++compressed[13];

  RawChars decompressed;
  EXPECT_FALSE(ZstdDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
}

TEST(ZstdEncoderDecoderTest, DecodeInvalidDataFailsStreaming) {
  auto compressed = BuildStreamingCompressed("some-data", 4096U);
  ASSERT_GT(compressed.size(), 4U);
  ++compressed[4];  // Corrupt the data
  RawChars decompressed;
  EXPECT_FALSE(ZstdDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_NE(std::string_view(decompressed), "some-data");
}

TEST(ZstdEncoderDecoderTest, StreamingSmallOutputBufferDrainsAndRoundTrips) {
  // Create a patterned payload large enough to force multiple compressStream2 calls
  // when the encoder is given a very small output buffer.
  const std::string payload = test::MakePatternedPayload(1024);

  CompressionConfig cfg;
  ZstdEncoder encoder(buf, cfg.zstd);
  auto ctx = encoder.makeContext();
  RawChars compressed;
  const auto produced = ctx->encodeChunk(std::string_view(payload));
  compressed.append(produced);
  const auto tail = ctx->encodeChunk({});
  compressed.append(tail);

  RawChars decompressed;
  ASSERT_TRUE(ZstdDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

TEST(ZstdEncoderDecoderTest, StreamingRandomIncompressibleForcesMultipleIterations) {
  // Incompressible payload to force encoder to iterate and grow output as needed.
  const RawBytes payload = test::MakeRandomPayload(256UL * 1024);

#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  static constexpr std::size_t kChunkSize = 8UL;
#else
  static constexpr std::size_t kChunkSize = 1UL;  // small to force multiple iterations; encoder will grow as needed
#endif
  CompressionConfig cfg;
  cfg.zstd.windowLog = 15;
  ZstdEncoder encoder(buf, cfg.zstd);
  auto ctx = encoder.makeContext();
  RawChars compressed;

  const auto produced =
      ctx->encodeChunk(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
  compressed.append(produced);
  const auto tail = ctx->encodeChunk({});
  compressed.append(tail);

  // Expect more than one chunk worth of output, implying multiple loop iterations.
  ASSERT_GT(compressed.size(), kChunkSize);

  RawChars decompressed;
  ASSERT_TRUE(ZstdDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));

  EXPECT_EQ(decompressed.size(), payload.size());
  EXPECT_EQ(std::memcmp(decompressed.data(), payload.data(), payload.size()), 0);
}

TEST(ZstdEncoderDecoderTest, RepeatedDecompressDoesNotGrowCapacity) {
  CompressionConfig cfg;
  ZstdEncoder encoder(buf, cfg.zstd);
  RawChars compressed;
  EncodeFull(encoder, "Zstd keeps strings sharp.", compressed, kExtraCapacity);

  RawChars decompressed;
  ASSERT_TRUE(ZstdDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  const auto cap1 = decompressed.capacity();
  ASSERT_GT(cap1, 0U);

  decompressed.clear();
  ASSERT_TRUE(ZstdDecoder::decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  const auto cap2 = decompressed.capacity();

  EXPECT_EQ(cap2, cap1);
}

}  // namespace aeronet