#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zstd-decoder.hpp"
#include "aeronet/zstd-encoder.hpp"

#if AERONET_WANT_MALLOC_OVERRIDES
#include <new>
#endif

namespace aeronet {

namespace {

constexpr uint32_t kDecoderChunkSize = 512;
constexpr std::size_t kExtraCapacity = 0;
constexpr std::size_t kMaxPlainBytes = 4UL * 1024 * 1024;

vector<std::string> SamplePayloads() {
  vector<std::string> payloads;
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
  const auto result = encoder.encodeFull(payload, out.capacity(), out.data());
  ASSERT_FALSE(result.hasError());
  out.setSize(result.writtenIfNoError());
}

void ExpectOneShotRoundTrip(std::string_view payload) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  RawChars compressed;
  EncodeFull(encoder, payload, compressed, kExtraCapacity);

  ZstdDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingRoundTrip(std::string_view payload, std::size_t split) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  const auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, split);
  ASSERT_FALSE(compressed.empty());
  ZstdDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingDecoderRoundTrip(std::string_view payload, std::size_t split) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  const auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, 4096U);
  ZstdDecoder decoder;
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
  }
  ASSERT_TRUE(ctx->decompressChunk({}, true, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

}  // namespace

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(ZstdEncoderDecoderTest, MallocConstructorFails) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(), "some-data", 4096U);
  ZstdDecoder decoder;
  test::FailNextRealloc();
  RawChars buf;
  // Zstd's custom allocator is backed by BufferCache, which currently fails through realloc.
  EXPECT_THROW(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, buf), std::bad_alloc);
}

TEST(ZstdEncoderDecoderTest, ZstdContextInitFails) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  test::FailNextRealloc();
  EXPECT_THROW(encoder.makeContext(), std::bad_alloc);
}

TEST(ZstdEncoderDecoderTest, EncodeFails) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  RawChars buf(std::string_view{"some-data"}.size());
  EXPECT_TRUE(encoder.encodeFull("some-data", 0UL, buf.data()).hasError());

  auto ctx = encoder.makeContext();
  RawChars out(ZSTD_compressBound(std::string_view{"some-data"}.size()));
  test::FailNextRealloc();
  EXPECT_TRUE(ctx->encodeChunk("some-data", out.capacity(), out.data()).hasError());
}

#endif

TEST(ZstdEncoderContext, MoveConstructor) {
  ZstdEncoderContext ctx1;

  auto& selfCtx = ctx1;
  ctx1 = std::move(selfCtx);  // self move should do nothing

  ZstdDecoderContext decoderCtx1;
  ZstdDecoderContext decoderCtx2(std::move(decoderCtx1));

  auto& selfDecoderCtx = decoderCtx2;
  decoderCtx2 = std::move(selfDecoderCtx);  // self move should do nothing

  ctx1.init(2, 15);
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

  ZstdEncoderContext ctx2(std::move(ctx1));
  ctx2.init(2, 15);
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

TEST(ZstdEncoderDecoderTest, EncodeFullRoundTripsPayloads) {
  for (const auto& payload : SamplePayloads()) {
    SCOPED_TRACE(testing::Message() << "payload bytes=" << payload.size());
    ExpectOneShotRoundTrip(payload);
  }
}

TEST(ZstdEncoderDecoderTest, ZstdEndWithoutEnoughBufferShouldFail) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  auto ctx = encoder.makeContext();

  // Provide a too-small buffer to end()
  EXPECT_TRUE(ctx->end(0UL, nullptr).hasError());
}

TEST(ZstdEncoderDecoderTest, MaxDecompressedBytesFull) {
  for (const auto& payload : SamplePayloads()) {
    CompressionConfig cfg;
    ZstdEncoder encoder(cfg.zstd);
    RawChars compressed;
    EncodeFull(encoder, payload, compressed, kExtraCapacity);

    ZstdDecoder decoder;
    RawChars decompressed;
    const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
    EXPECT_EQ(decoder.decompressFull(compressed, limit, kDecoderChunkSize, decompressed), payload.empty());
  }
}

TEST(ZstdEncoderDecoderTest, MaxDecompressedBytesStreaming) {
  for (const auto& payload : SamplePayloads()) {
    CompressionConfig cfg;
    ZstdEncoder encoder(cfg.zstd);
    const auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, 8U);
    ZstdDecoder decoder;
    RawChars decompressed;
    const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
    EXPECT_EQ(decoder.decompressFull(compressed, limit, kDecoderChunkSize, decompressed), payload.empty());
  }
}

TEST(ZstdEncoderDecoderTest, MaxCompressedBytesAndEndAreSane) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  auto ctx = encoder.makeContext();
  const std::string payload = test::MakePatternedPayload(1024);

  const auto maxChunk = ctx->minEncodeChunkCapacity(payload.size());
  ASSERT_GT(maxChunk, 0U);
  RawChars chunkOut(maxChunk);
  auto result = ctx->encodeChunk(payload, chunkOut.capacity(), chunkOut.data());
  ASSERT_FALSE(result.hasError());
  EXPECT_LE(result.writtenIfNoError(), maxChunk);

  RawChars tailOut(ctx->endChunkSize());
  while (true) {
    result = ctx->end(tailOut.capacity(), tailOut.data());
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() == 0) {
      break;
    }
    EXPECT_LE(result.writtenIfNoError(), tailOut.capacity());
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
  ZstdDecoder decoder;
  RawChars decompressed;
  EXPECT_FALSE(decoder.decompressFull(invalidData, kMaxPlainBytes, kDecoderChunkSize, decompressed));
}

TEST(ZstdEncoderDecoderTest, DecodeInvalidDataFailsFull) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  RawChars compressed;
  EncodeFull(encoder, std::string(512, 'A'), compressed, kExtraCapacity);

  // Corrupt the compressed data
  ASSERT_GT(compressed.size(), 13U);
  ++compressed[13];

  ZstdDecoder decoder;
  RawChars decompressed;
  EXPECT_FALSE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
}

TEST(ZstdEncoderDecoderTest, DecodeInvalidDataFailsStreaming) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(), "some-data", 4096U);
  ASSERT_GT(compressed.size(), 4U);
  ++compressed[4];  // Corrupt the data
  ZstdDecoder decoder;
  RawChars decompressed;
  EXPECT_FALSE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_NE(std::string_view(decompressed), "some-data");
}

TEST(ZstdEncoderDecoderTest, StreamingSmallOutputBufferDrainsAndRoundTrips) {
  // Create a patterned payload large enough to force multiple compressStream2 calls
  // when the encoder is given a very small output buffer.
  const std::string payload = test::MakePatternedPayload(1024);

  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  auto ctx = encoder.makeContext();
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

  ZstdDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

TEST(ZstdEncoderDecoderTest, StreamingRandomIncompressibleForcesMultipleIterations) {
  // Incompressible payload to force encoder to iterate and grow output as needed.
  const auto payload = test::MakeRandomPayload(64UL * 1024);

#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  static constexpr std::size_t kChunkSize = 8UL;
#else
  static constexpr std::size_t kChunkSize = 1UL;  // small to force multiple iterations; encoder will grow as needed
#endif
  CompressionConfig cfg;
  cfg.zstd.windowLog = 15;
  ZstdEncoder encoder(cfg.zstd);
  auto ctx = encoder.makeContext();
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

  ZstdDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));

  EXPECT_EQ(decompressed.size(), payload.size());
  EXPECT_EQ(std::memcmp(decompressed.data(), payload.data(), payload.size()), 0);
}

TEST(ZstdEncoderDecoderTest, RepeatedDecompressDoesNotGrowCapacity) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  RawChars compressed;
  EncodeFull(encoder, "Zstd keeps strings sharp.", compressed, kExtraCapacity);

  ZstdDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  const auto cap1 = decompressed.capacity();
  ASSERT_GT(cap1, 0U);

  decompressed.clear();
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  const auto cap2 = decompressed.capacity();

  EXPECT_EQ(cap2, cap1);
}

TEST(ZstdEncoderDecoderTest, EncodeChunkWithInsufficientOutputCapacity) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  auto ctx = encoder.makeContext();

  // Create a large input.
  std::string large(4096, 'X');

  // Try to encode with only 1 byte available.
  char tiny[1];
  const auto result = ctx->encodeChunk(large, sizeof(tiny), tiny);

  if (!result.hasError()) {
    // If zstd has not reported an error at encodeChunk time, it should report an error at end() time since the stream
    // can't be finalized with pending input.
    const auto endResult1 = ctx->end(sizeof(tiny) - result.writtenIfNoError(), tiny);
    if (!endResult1.hasError()) {
      const auto endResult2 = ctx->end(sizeof(tiny) - result.writtenIfNoError() - endResult1.writtenIfNoError(), tiny);
      EXPECT_TRUE(endResult2.hasError());
    }
  }
}

// A frame cut short is rejected, whether its decompressed size is declared (one-shot encoding) or not (streaming).
TEST(ZstdEncoderDecoderTest, TruncatedFrameIsRejected) {
  const std::string payload = test::MakePatternedPayload(100000);
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  RawChars oneShot;
  EncodeFull(encoder, payload, oneShot, kExtraCapacity);
  const auto streamed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, 4096U);
  for (const std::string_view frame : {std::string_view(oneShot), std::string_view(streamed)}) {
    for (const std::size_t cut : {std::size_t{1}, frame.size() / 2}) {
      ZstdDecoder decoder;
      RawChars decompressed;
      EXPECT_FALSE(
          decoder.decompressFull(frame.substr(0, frame.size() - cut), kMaxPlainBytes, kDecoderChunkSize, decompressed))
          << "cut " << cut;

      // Fed piece by piece, the frame only fails at the final chunk.
      auto* ctx = decoder.makeContext();
      decompressed.clear();
      const std::string_view truncated = frame.substr(0, frame.size() - cut);
      EXPECT_TRUE(ctx->decompressChunk(truncated.substr(0, truncated.size() - 1), false, kMaxPlainBytes,
                                       kDecoderChunkSize, decompressed));
      EXPECT_FALSE(ctx->decompressChunk(truncated.substr(truncated.size() - 1), true, kMaxPlainBytes, kDecoderChunkSize,
                                        decompressed));
    }
  }
}

// An empty chunk decodes nothing. When final, it tells whether the input fed so far ends with a complete frame: a
// caller can feed pieces without knowing which one is the last, then check the end of the content.
TEST(ZstdEncoderDecoderTest, EmptyFinalChunkTellsWhetherTheFrameIsComplete) {
  const std::string payload = test::MakePatternedPayload(20000);
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  const auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, 4096U);
  const std::string_view stream(compressed);
  ZstdDecoder decoder;
  auto* ctx = decoder.makeContext();
  RawChars decompressed;
  EXPECT_TRUE(ctx->decompressChunk({}, false, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_FALSE(ctx->decompressChunk({}, true, kMaxPlainBytes, kDecoderChunkSize, decompressed));  // nothing fed yet
  EXPECT_TRUE(decompressed.empty());
  // A context initialized again (by makeContext()) starts new content, so it is first reported incomplete.
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

// Decoding into an output buffer far smaller than the decoded blocks still yields the whole output, whether the frame
// comes in several chunks or as a single final one.
TEST(ZstdEncoderDecoderTest, FinalChunkFlushesTheWholeOutput) {
  const std::string payload(1UL << 20U, 'z');  // a few compressed bytes decode to whole 128 KiB blocks
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  const auto streamed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, payload.size());
  for (const uint32_t decoderChunkSize : {512U, 4096U}) {
    ZstdDecoder decoder;
    auto* ctx = decoder.makeContext();
    RawChars decompressed;
    const std::string_view view(streamed);
    ASSERT_TRUE(
        ctx->decompressChunk(view.substr(0, view.size() - 1), false, kMaxPlainBytes, decoderChunkSize, decompressed));
    ASSERT_TRUE(
        ctx->decompressChunk(view.substr(view.size() - 1), true, kMaxPlainBytes, decoderChunkSize, decompressed));
    EXPECT_EQ(decompressed.size(), payload.size()) << decoderChunkSize;
    EXPECT_TRUE(std::string_view(decompressed) == payload) << decoderChunkSize;

    // The whole frame as a single final chunk.
    ctx = decoder.makeContext();
    decompressed.clear();
    ASSERT_TRUE(ctx->decompressChunk(view, true, kMaxPlainBytes, decoderChunkSize, decompressed));
    EXPECT_EQ(decompressed.size(), payload.size()) << decoderChunkSize;
  }
}

// Content made of several frames (RFC 8878 §3.1) is decoded whole, wherever the input pieces start and end.
TEST(ZstdEncoderDecoderTest, ConcatenatedFramesAreDecoded) {
  const std::string first = test::MakePatternedPayload(70000);
  const std::string second(50000, 's');
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  RawChars oneShot;  // declares its decompressed size
  EncodeFull(encoder, first, oneShot, kExtraCapacity);
  const auto streamed = test::BuildStreamingCompressed(*encoder.makeContext(), second, 4096U);
  std::string frames(std::string_view{oneShot});
  frames.append(std::string_view{streamed});
  const std::string expected = first + second;

  ZstdDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(frames, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_TRUE(std::string_view(decompressed) == expected);

  for (const std::size_t split :
       {std::size_t{1}, std::size_t{7}, static_cast<std::size_t>(oneShot.size()), std::size_t{4096}}) {
    auto* ctx = decoder.makeContext();
    decompressed.clear();
    for (std::size_t off = 0; off < frames.size(); off += split) {
      const std::string_view piece = std::string_view(frames).substr(off, split);
      ASSERT_TRUE(ctx->decompressChunk(piece, off + piece.size() == frames.size(), kMaxPlainBytes, kDecoderChunkSize,
                                       decompressed))
          << "split " << split << " offset " << off;
    }
    EXPECT_TRUE(std::string_view(decompressed) == expected) << "split " << split;
  }

  // A truncated second frame is still rejected.
  EXPECT_FALSE(decoder.decompressFull(std::string_view(frames).substr(0, frames.size() - 1), kMaxPlainBytes,
                                      kDecoderChunkSize, decompressed));

  // Fed in pieces none of which is final, the content is complete at the end of each frame, and only there.
  const auto firstFrameSize = static_cast<std::size_t>(oneShot.size());
  for (const std::size_t split : {std::size_t{1}, firstFrameSize}) {
    const test::FedStream fed = test::FeedNonFinal(*decoder.makeContext(), frames, split, kDecoderChunkSize);
    EXPECT_TRUE(fed.ok) << split;
    EXPECT_EQ(fed.completeAt, (std::vector<std::size_t>{firstFrameSize, frames.size()})) << split;
    EXPECT_TRUE(std::string_view(fed.decoded) == expected) << split;
  }
}

TEST(ZstdEncoderDecoderTest, DeclaresMoreThan) {
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  RawChars oneShot;
  EncodeFull(encoder, std::string(1000UL, 'd'), oneShot, kExtraCapacity);
  EXPECT_TRUE(ZstdDecoder::DeclaresMoreThan(oneShot, 999));
  EXPECT_FALSE(ZstdDecoder::DeclaresMoreThan(oneShot, 1000));
  // Unknown declared size (streaming encoder) or invalid frame: nothing is declared.
  const auto streamed = test::BuildStreamingCompressed(*encoder.makeContext(), std::string(1000, 'd'), 64U);
  EXPECT_FALSE(ZstdDecoder::DeclaresMoreThan(streamed, 1));
  EXPECT_FALSE(ZstdDecoder::DeclaresMoreThan("not zstd at all", 1));
}

// maxDecompressedBytes holds when the output buffer already has room beyond it, as a reused buffer has: a frame
// decoding to exactly the limit is accepted, one byte more is rejected, in one shot or fed piece by piece.
TEST(ZstdEncoderDecoderTest, MaxDecompressedBytesWithSpareOutputCapacity) {
  const std::string payload = test::MakePatternedPayload(100000);
  CompressionConfig cfg;
  ZstdEncoder encoder(cfg.zstd);
  // Streaming encoding: the frame does not declare its decompressed size, so the decoder has to stop the output.
  const auto streamed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, 4096U);
  const std::string_view view(streamed);
  for (const std::size_t pieceSize : {view.size(), std::size_t{100}}) {
    for (const std::size_t limit : {payload.size() - 1, payload.size()}) {
      ZstdDecoder decoder;
      auto* ctx = decoder.makeContext();
      RawChars decompressed(4 * payload.size());
      bool ok = true;
      for (std::size_t off = 0; ok && off < view.size(); off += pieceSize) {
        const std::string_view piece = view.substr(off, pieceSize);
        ok = ctx->decompressChunk(piece, off + piece.size() == view.size(), limit - decompressed.size(),
                                  kDecoderChunkSize, decompressed);
      }
      EXPECT_EQ(ok, limit == payload.size()) << "piece " << pieceSize << " limit " << limit;
      EXPECT_LE(decompressed.size(), limit + 1);
    }
  }
}

}  // namespace aeronet