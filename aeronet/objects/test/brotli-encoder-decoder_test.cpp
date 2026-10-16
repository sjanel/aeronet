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

#include "aeronet/brotli-decoder.hpp"
#include "aeronet/brotli-encoder.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/vector.hpp"
#include "brotli/encode.h"

#if AERONET_WANT_MALLOC_OVERRIDES
#include <new>
#endif

namespace aeronet {

namespace {

constexpr uint32_t kDecoderChunkSize = 256;
constexpr std::size_t kExtraCapacity = 0;
constexpr std::size_t kMaxPlainBytes = 2UL * 1024 * 1024;

vector<std::string> SamplePayloads() {
  vector<std::string> payloads;
  payloads.reserve(4U);
  payloads.emplace_back("");
  payloads.emplace_back("Hello, Brotli compression!");
  payloads.emplace_back(512U, 'A');
  payloads.emplace_back(test::MakePatternedPayload(128UL * 1024UL));
  return payloads;
}

void EncodeFull(BrotliEncoder& encoder, std::string_view payload, RawChars& out, std::size_t extraCapacity = 0) {
  out.clear();
  out.reserve(BrotliEncoderMaxCompressedSize(payload.size()) + extraCapacity);
  const auto result = encoder.encodeFull(payload, out.capacity(), out.data());
  ASSERT_FALSE(result.hasError());
  out.setSize(result.writtenIfNoError());
}

void ExpectOneShotRoundTrip(BrotliEncoder& encoder, std::string_view payload) {
  RawChars compressed;
  EncodeFull(encoder, payload, compressed, kExtraCapacity);
  BrotliDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingRoundTrip(BrotliEncoder& encoder, std::string_view payload, std::size_t split) {
  auto ctx = encoder.makeContext();
  const auto compressed = test::BuildStreamingCompressed(*ctx, payload, split);
  BrotliDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

void ExpectStreamingDecoderRoundTrip(BrotliEncoder& encoder, std::string_view payload, std::size_t split) {
  const auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, 4096U);
  BrotliDecoder decoder;
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

TEST(BrotliDecoderTest, MallocConstructorFails) {
  // Simulate malloc failure during BrotliDecoderCreateInstance
  RawChars buf;
  test::FailNextRealloc();
  EXPECT_THROW((BrotliDecoder{}.decompressFull("some-data", kMaxPlainBytes, kDecoderChunkSize, buf)), std::bad_alloc);

  // Test encoder init failure
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  test::FailNextMalloc();
  EXPECT_THROW(encoder.makeContext(), std::bad_alloc);
}

#endif

TEST(BrotliEncoderContext, MoveOperations) {
  BrotliDecoderContext ctx1;

  ctx1.init();
  BrotliDecoderContext ctx2(std::move(ctx1));

  BrotliDecoderContext ctx3;
  ctx3.init();
  ctx3 = std::move(ctx2);

  auto& selfCtx = ctx3;
  ctx3 = std::move(selfCtx);  // self move should do nothing
}

TEST(BrotliEncoderTest, MoveConstructInitializedEncoder) {
  CompressionConfig cfg;
  BrotliEncoder initialized(cfg.brotli);
  RawChars compressed;
  EncodeFull(initialized, "prime the source allocator", compressed);

  BrotliEncoder moved(std::move(initialized));

  ExpectOneShotRoundTrip(moved, "works after move construction");
}

TEST(BrotliEncoderTest, MoveAssignOverInitializedEncoder) {
  CompressionConfig cfg;
  BrotliEncoder initialized(cfg.brotli);
  RawChars compressed;
  EncodeFull(initialized, "prime the destination allocator", compressed);

  BrotliEncoder replacement(cfg.brotli);
  EncodeFull(replacement, "prime the source allocator", compressed);
  initialized = std::move(replacement);

  ExpectOneShotRoundTrip(initialized, "works after move assignment");
}

TEST(BrotliEncoderDecoderTest, MoveConstructor) {
  BrotliScratch scratch;
  BrotliEncoderContext ctx1(scratch);
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

  BrotliEncoderContext ctx2(std::move(ctx1));

  auto& selfCtx = ctx2;
  ctx2 = std::move(selfCtx);  // self move should do nothing

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

TEST(BrotliEncoderDecoderTest, BrotliEndWithoutEnoughBufferShouldFail) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  auto ctx = encoder.makeContext();

  // Provide a too-small buffer to end()
  const auto result = ctx->end(0UL, nullptr);
  EXPECT_TRUE(result.hasError());
}

TEST(BrotliEncoderDecoderTest, EncodeChunkAfterFinalizationReturnsZero) {
  // Finish the stream, then try to encode more data: should return -1 to signal an error.
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  auto ctx = encoder.makeContext();
  // Produce some initial data.

  RawChars chunkOut;
  const auto result = test::EncodeChunk(*ctx, "Test data", chunkOut);
  ASSERT_FALSE(result.hasError());

  // Finalize the stream.
  test::EndStream(*ctx, chunkOut);
  // Encoding after finalization should return -1 to signal an error.
  RawChars extra;
  extra.reserve(BrotliEncoderMaxCompressedSize(std::string_view{"More data"}.size()));
  EXPECT_TRUE(ctx->encodeChunk("More data", extra.capacity(), extra.data()).hasError());

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

  BrotliDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(decompressed.size(), 0U);
}

TEST(BrotliEncoderDecoderTest, MaxCompressedBytesAndEndAreSane) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  auto ctx = encoder.makeContext();
  const std::string payload = test::MakePatternedPayload(1024);

  const auto maxChunk = ctx->minEncodeChunkCapacity(payload.size());
  ASSERT_GT(maxChunk, 0U);
  RawChars chunkOut(maxChunk);
  const auto result = ctx->encodeChunk(payload, chunkOut.capacity(), chunkOut.data());
  ASSERT_FALSE(result.hasError());
  EXPECT_LE(result.writtenIfNoError(), maxChunk);

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

TEST(BrotliEncoderDecoderTest, MaxDecompressedBytes) {
  for (const auto& payload : SamplePayloads()) {
    CompressionConfig cfg;
    BrotliEncoder encoder(cfg.brotli);
    RawChars compressed;
    EncodeFull(encoder, payload, compressed, kExtraCapacity);

    BrotliDecoder decoder;
    RawChars decompressed;
    const std::size_t limit = payload.empty() ? 0 : payload.size() - 1;
    const bool isOK = decoder.decompressFull(compressed, limit, kDecoderChunkSize, decompressed);
    EXPECT_EQ(isOK, payload.empty());
    // Exceeding the limit is detected by decoding one more byte.
    EXPECT_EQ(decompressed, std::string_view(payload).substr(0, limit + 1));
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
      const auto result = test::EncodeChunk(*ctx, chunk, chunkOut);
      ASSERT_FALSE(result.hasError());
      if (result.writtenIfNoError() > 0) {
        streamingCompressed.append(chunkOut);
      }
    }
    test::EndStream(*ctx, streamingCompressed);

    // Decode both datas to ensure validity
    BrotliDecoder decoder;
    RawChars decompressedOneShot;
    RawChars decompressedStreaming;
    ASSERT_TRUE(decoder.decompressFull(std::string_view(oneShotCompressed), kMaxPlainBytes, kDecoderChunkSize,
                                       decompressedOneShot));
    ASSERT_TRUE(decoder.decompressFull(std::string_view(streamingCompressed), kMaxPlainBytes, kDecoderChunkSize,
                                       decompressedStreaming));
    EXPECT_EQ(std::string_view(decompressedOneShot), payload);
    EXPECT_EQ(std::string_view(decompressedStreaming), payload);
  }
}

TEST(BrotliEncoderDecoderTest, DecodeInvalidDataFails) {
  RawChars invalidData("NotAValidBrotliStream");
  BrotliDecoder decoder;
  RawChars decompressed;
  EXPECT_FALSE(decoder.decompressFull(invalidData, kMaxPlainBytes, kDecoderChunkSize, decompressed));
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
    const auto result = test::EncodeChunk(*ctx, std::string_view(payload), chunkOut);
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() > 0) {
      compressed.append(chunkOut);
    }
  }
  test::EndStream(*ctx, compressed);

  BrotliDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  EXPECT_EQ(std::string_view(decompressed), payload);
}

TEST(BrotliEncoderDecoderTest, StreamingRandomIncompressibleForcesMultipleIterations) {
  // Incompressible payload to force encoder to iterate and grow output as needed.
  const auto payload = test::MakeRandomPayload(64UL * 1024);

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
    const auto result = test::EncodeChunk(*ctx, payload, chunkOut);
    ASSERT_FALSE(result.hasError());
    if (result.writtenIfNoError() > 0) {
      compressed.append(chunkOut);
    }
  }
  test::EndStream(*ctx, compressed);

  // Expect more than one chunk worth of output, implying multiple loop iterations.
  ASSERT_GT(compressed.size(), kChunkSize);

  BrotliDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(compressed, kMaxPlainBytes, kDecoderChunkSize, decompressed));

  EXPECT_EQ(decompressed.size(), payload.size());
  EXPECT_EQ(std::memcmp(decompressed.data(), payload.data(), payload.size()), 0);
}

TEST(BrotliEncoderDecoderTest, RepeatedDecompressDoesNotGrowCapacity) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  RawChars compressed;
  EncodeFull(encoder, "Hello, Brotli compression!", compressed, kExtraCapacity);

  BrotliDecoder decoder;
  RawChars decompressed;
  ASSERT_TRUE(decoder.decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
  const auto cap1 = decompressed.capacity();
  ASSERT_GT(cap1, 0U);

  decompressed.clear();
  ASSERT_TRUE(decoder.decompressFull(std::string_view(compressed), kMaxPlainBytes, kDecoderChunkSize, decompressed));
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

  if (!result.hasError()) {
    // If brotli has not reported an error at encodeChunk time, it should report an error at end() time since the stream
    // can't be finalized with pending input.
    const auto endResult1 = ctx->end(sizeof(tiny) - result.writtenIfNoError(), tiny);
    if (!endResult1.hasError()) {
      const auto endResult2 = ctx->end(sizeof(tiny) - result.writtenIfNoError() - endResult1.writtenIfNoError(), tiny);
      EXPECT_TRUE(endResult2.hasError());
    }
  }
}

// A stream cut short is rejected, whether it was encoded in one shot or in streaming.
TEST(BrotliEncoderDecoderTest, TruncatedStreamIsRejected) {
  const std::string payload = test::MakePatternedPayload(100000);
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  RawChars oneShot;
  EncodeFull(encoder, payload, oneShot, kExtraCapacity);
  const auto streamed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, 4096U);
  for (const std::string_view stream : {std::string_view(oneShot), std::string_view(streamed)}) {
    for (const std::size_t cut : {std::size_t{1}, stream.size() / 2}) {
      BrotliDecoder decoder;
      RawChars decompressed;
      const std::string_view truncated = stream.substr(0, stream.size() - cut);
      EXPECT_FALSE(decoder.decompressFull(truncated, kMaxPlainBytes, kDecoderChunkSize, decompressed)) << "cut " << cut;

      // Fed piece by piece, the stream only fails at the final chunk.
      auto* ctx = decoder.makeContext();
      decompressed.clear();
      EXPECT_TRUE(ctx->decompressChunk(truncated.substr(0, truncated.size() - 1), false, kMaxPlainBytes,
                                       kDecoderChunkSize, decompressed));
      EXPECT_FALSE(ctx->decompressChunk(truncated.substr(truncated.size() - 1), true, kMaxPlainBytes, kDecoderChunkSize,
                                        decompressed));
    }
  }
}

// An empty chunk decodes nothing. When final, it tells whether the input fed so far completes the stream: a caller
// can feed pieces without knowing which one is the last, then check the end of the stream.
TEST(BrotliEncoderDecoderTest, EmptyFinalChunkTellsWhetherTheStreamIsComplete) {
  const std::string payload = test::MakePatternedPayload(20000);
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  const auto compressed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, 4096U);
  const std::string_view stream(compressed);
  BrotliDecoder decoder;
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

// Decoding into an output buffer far smaller than the decoded data still yields the whole output, whether the stream
// comes in several chunks or as a single final one.
TEST(BrotliEncoderDecoderTest, FinalChunkFlushesTheWholeOutput) {
  const std::string payload(1UL << 20U, 'b');  // a few compressed bytes decode to the whole payload
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  const auto streamed = test::BuildStreamingCompressed(*encoder.makeContext(), payload, payload.size());
  const std::string_view view(streamed);
  for (const uint32_t decoderChunkSize : {512U, 4096U}) {
    BrotliDecoder decoder;
    auto* ctx = decoder.makeContext();
    RawChars decompressed;
    ASSERT_TRUE(
        ctx->decompressChunk(view.substr(0, view.size() - 1), false, kMaxPlainBytes, decoderChunkSize, decompressed));
    ASSERT_TRUE(
        ctx->decompressChunk(view.substr(view.size() - 1), true, kMaxPlainBytes, decoderChunkSize, decompressed));
    EXPECT_TRUE(std::string_view(decompressed) == payload) << decoderChunkSize;

    // The whole stream as a single final chunk.
    ctx = decoder.makeContext();
    decompressed.clear();
    ASSERT_TRUE(ctx->decompressChunk(view, true, kMaxPlainBytes, decoderChunkSize, decompressed));
    EXPECT_TRUE(std::string_view(decompressed) == payload) << decoderChunkSize;
  }
}

// A stream fed in pieces whose decoded size comes within one decoder chunk of maxDecompressedBytes is accepted: the
// limit only stops an output that would exceed it.
TEST(BrotliEncoderDecoderTest, StreamingDecodeCloseToTheLimitIsAccepted) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  for (const RawChars& payload : {test::MakeRandomPayload(9000), RawChars(test::MakePatternedPayload(300000))}) {
    RawChars compressed;
    EncodeFull(encoder, std::string_view{payload}, compressed, kExtraCapacity);
    const std::string_view view{compressed};
    for (const std::size_t limit : {payload.size(), payload.size() + 1, payload.size() + kDecoderChunkSize}) {
      for (const std::size_t pieceSize : {std::size_t{1}, std::size_t{100}, std::size_t{4096}}) {
        BrotliDecoder decoder;
        auto* ctx = decoder.makeContext();
        RawChars decompressed;
        bool ok = true;
        for (std::size_t off = 0; ok && off < view.size(); off += pieceSize) {
          const std::string_view piece = view.substr(off, pieceSize);
          ok = ctx->decompressChunk(piece, off + piece.size() == view.size(), limit - decompressed.size(),
                                    kDecoderChunkSize, decompressed);
        }
        EXPECT_TRUE(ok) << "size " << payload.size() << " limit " << limit << " piece " << pieceSize;
        EXPECT_TRUE(std::string_view(decompressed) == std::string_view{payload});
      }
    }
  }
}

// Brotli streams cannot be concatenated (RFC 7932): bytes after the last meta-block are rejected.
TEST(BrotliEncoderDecoderTest, TrailingDataIsRejected) {
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  RawChars first;
  EncodeFull(encoder, test::MakePatternedPayload(70000), first, kExtraCapacity);
  RawChars second;
  EncodeFull(encoder, std::string(50000, 's'), second, kExtraCapacity);
  std::string streams(std::string_view{first});
  streams.append(std::string_view{second});

  BrotliDecoder decoder;
  RawChars decompressed;
  EXPECT_FALSE(decoder.decompressFull(streams, kMaxPlainBytes, kDecoderChunkSize, decompressed));

  // Fed piece by piece, the end of the first stream is found in a piece followed by more input.
  for (const std::size_t split : {static_cast<std::size_t>(7), static_cast<std::size_t>(first.size())}) {
    auto* ctx = decoder.makeContext();
    decompressed.clear();
    bool ok = true;
    for (std::size_t off = 0; ok && off < streams.size(); off += split) {
      const std::string_view piece = std::string_view(streams).substr(off, split);
      ok = ctx->decompressChunk(piece, off + piece.size() == streams.size(), kMaxPlainBytes, kDecoderChunkSize,
                                decompressed);
    }
    EXPECT_FALSE(ok) << "split " << split;
  }
}

// maxDecompressedBytes holds when the output buffer already has room beyond it, as a reused buffer has: a stream
// decoding to exactly the limit is accepted, one byte more is rejected, in one shot or fed piece by piece.
TEST(BrotliEncoderDecoderTest, MaxDecompressedBytesWithSpareOutputCapacity) {
  const std::string payload = test::MakePatternedPayload(100000);
  CompressionConfig cfg;
  BrotliEncoder encoder(cfg.brotli);
  RawChars compressed;
  EncodeFull(encoder, payload, compressed, kExtraCapacity);
  const std::string_view view(compressed);
  for (const std::size_t pieceSize : {view.size(), std::size_t{100}}) {
    for (const std::size_t limit : {payload.size() - 1, payload.size()}) {
      BrotliDecoder decoder;
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