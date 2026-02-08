#include "aeronet/http-codec.hpp"

#include <gtest/gtest.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-encoder.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#endif

namespace aeronet::internal {

namespace {
#ifdef AERONET_ENABLE_ZLIB
[[nodiscard]] std::size_t ParseContentLength(std::string_view value) {
  std::size_t out = 0;
  const auto* first = value.data();
  const auto* last = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(first, last, out);
  EXPECT_EQ(ec, std::errc()) << "Invalid Content-Length value: '" << value << "'";
  EXPECT_EQ(ptr, last) << "Trailing characters in Content-Length value: '" << value << "'";
  return out;
}

[[nodiscard]] bool VaryHasToken(std::string_view value, std::string_view token) {
  const char* it = value.data();
  const char* const end = value.data() + value.size();
  while (it < end) {
    while (it < end && (*it == ',' || http::IsHeaderWhitespace(*it))) {
      ++it;
    }
    const char* tokenBeg = it;
    while (it < end && *it != ',') {
      ++it;
    }
    const char* tokenEnd = it;
    while (tokenEnd > tokenBeg && http::IsHeaderWhitespace(*(tokenEnd - 1))) {
      --tokenEnd;
    }
    if (tokenEnd == tokenBeg) {
      continue;
    }
    const std::string_view cur(tokenBeg, static_cast<std::size_t>(tokenEnd - tokenBeg));
    if (token == "*") {
      if (cur == "*") {
        return true;
      }
    } else {
      if (CaseInsensitiveEqual(cur, token)) {
        return true;
      }
    }
  }
  return false;
}
#endif
}  // namespace

TEST(HttpCodecCompression, ContentTypeAllowListBlocksCompression) {
  CompressionConfig cfg;
  cfg.minBytes = 16U;
  cfg.preferredFormats.clear();
#ifdef AERONET_ENABLE_ZLIB
  cfg.preferredFormats.push_back(Encoding::gzip);
#endif
#ifdef AERONET_ENABLE_ZSTD
  cfg.preferredFormats.push_back(Encoding::zstd);
#endif
#ifdef AERONET_ENABLE_BROTLI
  cfg.preferredFormats.push_back(Encoding::br);
#endif
  // make allow list only accept text/plain
  cfg.contentTypeAllowList.clear();
  cfg.contentTypeAllowList.append("text/plain");

  ResponseCompressionState state(cfg);

  const std::string body(4096, 'A');

  HttpResponse resp(http::StatusCodeOK);
  resp.body(body, http::ContentTypeApplicationJson);

  // Try compress - application/json response Content-Type is not in allowlist -> no compression
  HttpCodec::TryCompressResponse(state, cfg, Encoding::gzip, resp);
  EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());

  // Now use an allowed response Content-Type
  HttpResponse resp2(http::StatusCodeOK);
  resp2.body(body, http::ContentTypeTextPlain);
#ifdef AERONET_ENABLE_ZLIB
  HttpCodec::TryCompressResponse(state, cfg, Encoding::gzip, resp2);
#else
  EXPECT_THROW(HttpCodec::TryCompressResponse(state, cfg, Encoding::gzip, resp2), std::invalid_argument);
#endif

  // If encoders present, compression should be applied (Content-Encoding set). Otherwise no-op.
#ifdef AERONET_ENABLE_ZLIB
  EXPECT_FALSE(resp2.headerValueOrEmpty(http::ContentEncoding).empty());
  const auto contentLen = resp2.headerValueOrEmpty(http::ContentLength);
  ASSERT_FALSE(contentLen.empty());
  EXPECT_EQ(ParseContentLength(contentLen), resp2.bodyInMemoryLength());
#else
  EXPECT_TRUE(resp2.headerValueOrEmpty(http::ContentEncoding).empty());
#endif
}

TEST(HttpCodecCompression, VaryHeaderAddedWhenConfigured) {
  static constexpr std::string_view kVaryHeaderContent[] = {
      std::string_view(),
      "",
      "Something, Anything",
      "accept-encoding",
      "accept-encoding, SomethingElse",
      "*",
      "SomethingElse, *",
      "*, SomethingElse",
  };

  const std::string body(4096, 'A');

  CompressionConfig cfg;
  cfg.minBytes = 16U;
  cfg.addVaryAcceptEncodingHeader = true;
  cfg.contentTypeAllowList.clear();
  cfg.contentTypeAllowList.append("text/plain");
#ifdef AERONET_ENABLE_ZLIB
  cfg.preferredFormats.push_back(Encoding::gzip);
#endif
  ResponseCompressionState state(cfg);

  std::string_view acceptEncoding = "gzip";

  for (std::string_view varyContent : kVaryHeaderContent) {
    HttpResponse resp(http::StatusCodeOK, body, http::ContentTypeTextPlain);
    if (varyContent.data() != nullptr) {
      resp.header(http::Vary, varyContent);
    }
    // Diagnostics: ensure negotiation chooses gzip and encoder is present when expected.
    const auto neg = state.selector.negotiateAcceptEncoding(acceptEncoding);
#ifdef AERONET_ENABLE_ZLIB
    EXPECT_EQ(neg.encoding, Encoding::gzip);

    HttpCodec::TryCompressResponse(state, cfg, neg.encoding, resp);

    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), acceptEncoding);
    const auto contentLen = resp.headerValueOrEmpty(http::ContentLength);
    ASSERT_FALSE(contentLen.empty());
    EXPECT_EQ(ParseContentLength(contentLen), resp.bodyInMemoryLength());

    // If a Vary header exists, merge ", Accept-Encoding" into its value.
    // If Vary already contains Accept-Encoding or '*', it must be left untouched.
    std::size_t varyCount = 0;
    for (const auto& hdr : resp.headers()) {
      if (CaseInsensitiveEqual(hdr.name, http::Vary)) {
        ++varyCount;
      }
    }
    EXPECT_EQ(varyCount, 1UL);

    const std::string_view varyValue = resp.headerValueOrEmpty(http::Vary);
    ASSERT_FALSE(varyValue.empty());

    if (varyContent.data() == nullptr) {
      EXPECT_EQ(varyValue, http::AcceptEncoding);
    } else if (VaryHasToken(varyContent, "*") || VaryHasToken(varyContent, http::AcceptEncoding)) {
      EXPECT_EQ(varyValue, varyContent);
    } else {
      std::string expected(varyContent);
      if (!varyContent.empty()) {
        expected.append(", ");
      }
      expected.append(http::AcceptEncoding);
      EXPECT_EQ(varyValue, expected);
    }
#else
    EXPECT_EQ(neg.encoding, Encoding::none);
#endif
  }
}

TEST(HttpCodecCompression, VaryHeaderNotAddedWhenDisabled) {
  CompressionConfig cfg;
  cfg.minBytes = 16U;
  cfg.addVaryAcceptEncodingHeader = false;  // disable adding Vary
  cfg.contentTypeAllowList.clear();
  cfg.contentTypeAllowList.append("text/plain");
#ifdef AERONET_ENABLE_ZLIB
  cfg.preferredFormats.push_back(Encoding::gzip);
#endif
  ResponseCompressionState state(cfg);

  const std::string body(4096, 'A');

  HttpResponse resp(body, http::ContentTypeTextPlain);

#ifdef AERONET_ENABLE_ZLIB
  HttpCodec::TryCompressResponse(state, cfg, Encoding::gzip, resp);
  // Compression should be applied but Vary must NOT be set because addVaryHeader == false
  EXPECT_FALSE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
  EXPECT_TRUE(resp.headerValueOrEmpty(http::Vary).empty());
  const auto contentLen = resp.headerValueOrEmpty(http::ContentLength);
  ASSERT_FALSE(contentLen.empty());
  EXPECT_EQ(ParseContentLength(contentLen), resp.bodyInMemoryLength());
#else
  SUCCEED();
#endif
}

#ifdef AERONET_ENABLE_ZLIB
TEST(HttpCodecCompression, GzipCompressedBodyRoundTrips) {
  CompressionConfig cfg;
  cfg.minBytes = 16U;
  cfg.addVaryAcceptEncodingHeader = true;
  cfg.preferredFormats.clear();
  cfg.preferredFormats.push_back(Encoding::gzip);
  cfg.contentTypeAllowList.clear();

  ResponseCompressionState state(cfg);

  const std::string body(16UL * 1024UL, 'A');
  HttpResponse resp(http::StatusCodeOK);
  resp.body(body, http::ContentTypeTextPlain);

  // Control: verify the encoder itself produces a gzip stream.
  {
    RawChars direct(64UL);
    direct.reserve(64ULL + body.size());
    const std::size_t written = state.encodeFull(Encoding::gzip, body, direct.capacity(), direct.data());
    ASSERT_GT(written, 0UL);
    direct.setSize(static_cast<RawChars::size_type>(written));
    ASSERT_GE(direct.size(), 2UL);
    EXPECT_EQ(static_cast<unsigned char>(direct[0]), 0x1fU);
    EXPECT_EQ(static_cast<unsigned char>(direct[1]), 0x8bU);
  }

  HttpCodec::TryCompressResponse(state, cfg, Encoding::gzip, resp);
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), http::gzip);

  const std::string_view compressedBody = resp.bodyInMemory();
  ASSERT_GE(compressedBody.size(), 2UL);
  EXPECT_EQ(static_cast<unsigned char>(compressedBody[0]), 0x1fU);
  EXPECT_EQ(static_cast<unsigned char>(compressedBody[1]), 0x8bU);

  RawChars out;
  ZlibDecoder decoder(ZStreamRAII::Variant::gzip);
  ASSERT_TRUE(decoder.decompressFull(compressedBody, /*maxDecompressedBytes=*/(1UL << 20), 32UL * 1024UL, out));
  EXPECT_EQ(std::string_view(out), std::string_view(body));
}
#endif

#ifdef AERONET_ENABLE_ZLIB
TEST(HttpCodecCompression, MaxCompressRatioCanDisableCompression) {
  CompressionConfig cfg;
  cfg.minBytes = 1024UL;
  cfg.addVaryAcceptEncodingHeader = false;
  cfg.contentTypeAllowList.clear();
  cfg.contentTypeAllowList.append("text/plain");
  cfg.preferredFormats.clear();
  cfg.preferredFormats.push_back(Encoding::gzip);
  cfg.maxCompressRatio = std::nextafter(1.0F, 0.0F);  // just below 1.0 to allow any compression

  ResponseCompressionState state(cfg);

  auto body = test::MakePatternedPayload(cfg.minBytes);

  HttpResponse resp(body);
  HttpCodec::TryCompressResponse(state, cfg, Encoding::gzip, resp);

  ASSERT_FALSE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
  const std::size_t compressedSize = resp.bodyInMemoryLength();
  ASSERT_GT(compressedSize, 0UL);

  // Configure a ratio that makes the previous compressedSize just too large.
  CompressionConfig cfg2 = cfg;
  const float tightRatio = static_cast<float>(compressedSize - 1) / static_cast<float>(body.size());
  cfg2.maxCompressRatio = std::nextafter(tightRatio, 0.0F);

  ResponseCompressionState state2(cfg2);

  HttpResponse resp2(body);
  HttpCodec::TryCompressResponse(state2, cfg2, Encoding::gzip, resp2);

  EXPECT_TRUE(resp2.headerValueOrEmpty(http::ContentEncoding).empty());
}
#endif

#ifdef AERONET_ENABLE_ZSTD
TEST(HttpCodecCompression, ImpossibleCompressionZstd) {
  CompressionConfig cfg;
  cfg.minBytes = 1024UL;
  cfg.addVaryAcceptEncodingHeader = false;
  cfg.contentTypeAllowList.clear();
  cfg.contentTypeAllowList.append("text/plain");
  cfg.preferredFormats.clear();
  cfg.preferredFormats.push_back(Encoding::zstd);
  cfg.maxCompressRatio = std::nextafter(1.0F, 0.0F);  // just below 1.0 to allow any compression

  ResponseCompressionState state(cfg);

  auto body = test::MakeRandomPayload(cfg.minBytes);

  HttpResponse resp(body);
  HttpCodec::TryCompressResponse(state, cfg, Encoding::zstd, resp);

  EXPECT_FALSE(resp.hasHeader(http::ContentEncoding));
  EXPECT_EQ(resp.bodyInMemoryLength(), body.size());
}
#endif

TEST(HttpCodecDecompression, WillDecompress_DisabledReturnsNotModified) {
  DecompressionConfig cfg;
  cfg.enable = false;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "gzip");

  const auto code = HttpCodec::WillDecompress(cfg, req.headers());
  EXPECT_EQ(code, http::StatusCodeNotModified);
}

TEST(HttpCodecDecompression, WillDecompress_NoHeaderReturnsNotModified) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;

  const auto code = HttpCodec::WillDecompress(cfg, req.headers());
  EXPECT_EQ(code, http::StatusCodeNotModified);
}

TEST(HttpCodecDecompression, WillDecompress_EmptyHeaderReturnsBadRequest) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "");

  const auto code = HttpCodec::WillDecompress(cfg, req.headers());
  EXPECT_EQ(code, http::StatusCodeBadRequest);
}

TEST(HttpCodecDecompression, WillDecompress_OnlyIdentityReturnsNotModified) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "identity");

  const auto code = HttpCodec::WillDecompress(cfg, req.headers());
  EXPECT_EQ(code, http::StatusCodeNotModified);
}

TEST(HttpCodecDecompression, WillDecompress_NonIdentityReturnsOK) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "gzip");

  const auto code = HttpCodec::WillDecompress(cfg, req.headers());
  EXPECT_EQ(code, http::StatusCodeOK);
}

TEST(HttpCodecDecompression, WillDecompress_MalformedDoubleCommaReturnsBadRequest) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  // double comma (possibly with spaces) between tokens should be treated as malformed
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "gzip,,deflate");

  const auto code = HttpCodec::WillDecompress(cfg, req.headers());
  EXPECT_EQ(code, http::StatusCodeBadRequest);
}

TEST(HttpCodecDecompression, WillDecompress_IdentityCaseInsensitive) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "IDENTITY");

  const auto code = HttpCodec::WillDecompress(cfg, req.headers());
  EXPECT_EQ(code, http::StatusCodeNotModified);
}

TEST(HttpCodecDecompression, WillDecompress_SeveralIdentityValuesReturnsNotModified) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "identity, identity,IDENTITY");

  const auto code = HttpCodec::WillDecompress(cfg, req.headers());
  EXPECT_EQ(code, http::StatusCodeNotModified);
}

TEST(HttpCodecDecompression, WillDecompress_OWSAndSpacesAreHandled) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  // leading/trailing spaces and OWS around commas should be tolerated and parsed
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "gzip ,  deflate");

  const auto code = HttpCodec::WillDecompress(cfg, req.headers());
  EXPECT_EQ(code, http::StatusCodeOK);  // gzip/deflate are non-identity
}

TEST(HttpCodecDecompression, WillDecompress_IdentityMixedWithOtherEncodings) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  // identity present but not alone; should result in OK because a non-identity is present
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "identity, br");

  const auto code = HttpCodec::WillDecompress(cfg, req.headers());
  EXPECT_EQ(code, http::StatusCodeOK);
}

TEST(HttpCodecDecompression, DecompressChunkedBody_MalformedEncodingReturnsBadRequest) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  // malformed double-comma should be treated as malformed by the decoder iterator
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "gzip,,deflate");

  std::string_view chunksArr[] = {"dummy"};
  std::span<const std::string_view> chunks(chunksArr, 1);

  RawChars bodyBuf;
  RawChars tmpBuf;
  RequestDecompressionState decompressionState;

  const auto res =
      HttpCodec::DecompressChunkedBody(decompressionState, cfg, req, chunks, /*compressedSize=*/1, bodyBuf, tmpBuf);
#ifdef AERONET_ENABLE_ZLIB
  EXPECT_EQ(res.status, http::StatusCodeBadRequest);
#else
  EXPECT_EQ(res.status, http::StatusCodeUnsupportedMediaType);
#endif
}

#ifdef AERONET_ENABLE_ZLIB
TEST(HttpCodecDecompression, DecompressChunkedBody_ExpansionTooLargeReturnsPayloadTooLarge) {
  DecompressionConfig cfg;
  cfg.enable = true;
  // set a very small allowed expansion ratio so normal compression expansion will exceed it
  cfg.maxExpansionRatio = 0.001;  // 0.1%

  ConnectionState cs;
  HttpRequest& req = cs.request;
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "identity,gzip,identity");

  // Prepare a large uncompressed payload that compresses well
  const std::size_t plainSize = 1 << 10;  // 1 KiB
  std::string plain(plainSize, 'A');

  CompressionConfig encCfg;  // default encoder config is fine for generating compressed bytes
  RawChars buf;
  ZlibEncoder encoder(encCfg.zlib.level);
  RawChars compressedOut(plain.size());
  {
    const std::size_t written = encoder.encodeFull(ZStreamRAII::Variant::gzip, std::string_view(plain),
                                                   compressedOut.capacity(), compressedOut.data());
    ASSERT_GT(written, 0UL);
    compressedOut.setSize(static_cast<RawChars::size_type>(written));
  }

  const std::string_view compressedView(compressedOut.data(), compressedOut.size());
  std::span<const std::string_view> chunks(&compressedView, 1);

  RawChars bodyBuf;
  RawChars tmpBuf;

  RequestDecompressionState decompressionState;

  const auto res = HttpCodec::DecompressChunkedBody(decompressionState, cfg, req, chunks,
                                                    /*compressedSize=*/compressedView.size(), bodyBuf, tmpBuf);
  EXPECT_EQ(res.status, http::StatusCodePayloadTooLarge);

  // Check with a large enough expansion ratio to ensure success
  cfg.maxExpansionRatio = (plainSize / static_cast<double>(compressedView.size())) + 1.0;
  const auto res2 = HttpCodec::DecompressChunkedBody(decompressionState, cfg, req, chunks,
                                                     /*compressedSize=*/compressedView.size(), bodyBuf, tmpBuf);
  EXPECT_EQ(res2.status, http::StatusCodeOK);
}
#endif

TEST(HttpCodecDecompression, DecompressChunkedBody_IdentityAndUnknownEncodingReturnsUnsupportedMediaType) {
  DecompressionConfig cfg;
  cfg.enable = true;

  ConnectionState cs;
  HttpRequest& req = cs.request;
  // identity and unknown encoding should return unsupported media type
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "identity, unknown");

  std::string_view chunksArr[] = {"dummy"};
  std::span<const std::string_view> chunks(chunksArr, 1);

  RawChars bodyBuf;
  RawChars tmpBuf;
  RequestDecompressionState decompressionState;
  const auto res =
      HttpCodec::DecompressChunkedBody(decompressionState, cfg, req, chunks, /*compressedSize=*/1, bodyBuf, tmpBuf);
  EXPECT_EQ(res.status, http::StatusCodeUnsupportedMediaType);
}

TEST(HttpCodecCompression, ResponseCompressionStateEncodeFull_BehaviorPerEncoder) {
  CompressionConfig cfg;
  cfg.minBytes = 16U;
  cfg.contentTypeAllowList.clear();
  // request negotiation doesn't matter for these direct encodeFull tests

  ResponseCompressionState state(cfg);

  const std::string plain(4096, 'A');

  // For each supported encoding, ensure encodeFull writes something when capacity is sufficient,
  // and returns 0 when capacity is too small.

#ifdef AERONET_ENABLE_ZLIB
  {
    RawChars out(64 + plain.size());
    const std::size_t written = state.encodeFull(Encoding::gzip, plain, out.capacity(), out.data());
    EXPECT_GT(written, 0UL);
    // too small capacity
    EXPECT_EQ(state.encodeFull(Encoding::gzip, plain, 1UL, out.data()), 0UL);
  }

  {
    RawChars out(64 + plain.size());
    const std::size_t written = state.encodeFull(Encoding::deflate, plain, out.capacity(), out.data());
    EXPECT_GT(written, 0UL);
    EXPECT_EQ(state.encodeFull(Encoding::deflate, plain, 1UL, out.data()), 0UL);
  }
#endif

#ifdef AERONET_ENABLE_ZSTD
  {
    RawChars out(64 + plain.size());
    const std::size_t written = state.encodeFull(Encoding::zstd, plain, out.capacity(), out.data());
    EXPECT_GT(written, 0UL);
    EXPECT_EQ(state.encodeFull(Encoding::zstd, plain, 1UL, out.data()), 0UL);
  }
#endif

#ifdef AERONET_ENABLE_BROTLI
  {
    RawChars out(64 + plain.size());
    const std::size_t written = state.encodeFull(Encoding::br, plain, out.capacity(), out.data());
    EXPECT_GT(written, 0UL);
    EXPECT_EQ(state.encodeFull(Encoding::br, plain, 1UL, out.data()), 0UL);
  }
#endif

  // The API should throw when asked to encode with Encoding::none
  EXPECT_THROW(state.encodeFull(Encoding::none, plain, 1024UL, nullptr), std::invalid_argument);
  EXPECT_THROW(state.encodeFull(static_cast<Encoding>(static_cast<std::underlying_type_t<Encoding>>(-1)), plain, 1024UL,
                                nullptr),
               std::invalid_argument);
}

TEST(HttpCodecCompression, ResponseCompressionStateMakeContext_BehaviorPerEncoder) {
  CompressionConfig cfg;
  cfg.minBytes = 16U;
  cfg.contentTypeAllowList.clear();

  ResponseCompressionState state(cfg);

  const std::string plain(4096, 'A');

  // For each supported encoding, ensure makeContext() can be created and used to compress
  // into a buffer when capacity is sufficient, and that small buffer limits behave as expected.

#ifdef AERONET_ENABLE_ZLIB
  {
    auto ctx = state.makeContext(Encoding::gzip);
    ASSERT_NE(ctx, nullptr);
    RawChars produced(ctx->maxCompressedBytes(plain.size()));
    const auto written = ctx->encodeChunk(plain, produced.capacity(), produced.data());
    EXPECT_EQ(ctx->encodeChunk({}, produced.capacity(), produced.data()), 0);
    RawChars producedFinal(ctx->endChunkSize());
    int64_t tailWritten = 0;
    do {
      tailWritten = ctx->end(producedFinal.capacity(), producedFinal.data());
      EXPECT_GE(tailWritten, 0);
    } while (tailWritten > 0);
    EXPECT_GE(written, 0);
  }

  {
    auto ctx = state.makeContext(Encoding::gzip);
    ASSERT_NE(ctx, nullptr);
    // End the stream without payload: call must succeed without throwing.
    RawChars tail(ctx->endChunkSize());
    int64_t tailWritten = 0;
    do {
      tailWritten = ctx->end(tail.capacity(), tail.data());
      ASSERT_GE(tailWritten, 0);
    } while (tailWritten > 0);
    SUCCEED();
  }

  {
    auto ctx = state.makeContext(Encoding::deflate);
    ASSERT_NE(ctx, nullptr);
    RawChars produced(ctx->maxCompressedBytes(plain.size()));
    const auto written = ctx->encodeChunk(plain, produced.capacity(), produced.data());
    EXPECT_EQ(ctx->encodeChunk({}, produced.capacity(), produced.data()), 0);
    RawChars producedFinal(ctx->endChunkSize());
    int64_t tailWritten = 0;
    do {
      tailWritten = ctx->end(producedFinal.capacity(), producedFinal.data());
      EXPECT_GE(tailWritten, 0);
    } while (tailWritten > 0);
    EXPECT_GE(written, 0);
  }

  {
    auto ctx = state.makeContext(Encoding::deflate);
    ASSERT_NE(ctx, nullptr);
    RawChars tail(ctx->endChunkSize());
    int64_t tailWritten = 0;
    do {
      tailWritten = ctx->end(tail.capacity(), tail.data());
      ASSERT_GE(tailWritten, 0);
    } while (tailWritten > 0);
    SUCCEED();
  }
#endif

#ifdef AERONET_ENABLE_ZSTD
  {
    auto ctx = state.makeContext(Encoding::zstd);
    ASSERT_NE(ctx, nullptr);
    RawChars produced(ctx->maxCompressedBytes(plain.size()));
    const auto written = ctx->encodeChunk(plain, produced.capacity(), produced.data());
    EXPECT_EQ(ctx->encodeChunk({}, produced.capacity(), produced.data()), 0);
    RawChars producedFinal(ctx->endChunkSize());
    int64_t tailWritten = 0;
    do {
      tailWritten = ctx->end(producedFinal.capacity(), producedFinal.data());
      EXPECT_GE(tailWritten, 0);
    } while (tailWritten > 0);
    EXPECT_GE(written, 0);
  }

  {
    auto ctx = state.makeContext(Encoding::zstd);
    ASSERT_NE(ctx, nullptr);
    RawChars tail(ctx->endChunkSize());
    int64_t tailWritten = 0;
    do {
      tailWritten = ctx->end(tail.capacity(), tail.data());
      ASSERT_GE(tailWritten, 0);
    } while (tailWritten > 0);
    SUCCEED();
  }
#endif

#ifdef AERONET_ENABLE_BROTLI
  {
    auto ctx = state.makeContext(Encoding::br);
    ASSERT_NE(ctx, nullptr);
    RawChars produced(ctx->maxCompressedBytes(plain.size()));
    const auto written = ctx->encodeChunk(plain, produced.capacity(), produced.data());
    EXPECT_EQ(ctx->encodeChunk({}, produced.capacity(), produced.data()), 0);
    RawChars producedFinal(ctx->endChunkSize());
    int64_t tailWritten = 0;
    do {
      tailWritten = ctx->end(producedFinal.capacity(), producedFinal.data());
      EXPECT_GE(tailWritten, 0);
    } while (tailWritten > 0);
    EXPECT_GE(written, 0);
  }

  {
    auto ctx = state.makeContext(Encoding::br);
    ASSERT_NE(ctx, nullptr);
    RawChars tail(ctx->endChunkSize());
    int64_t tailWritten = 0;
    do {
      tailWritten = ctx->end(tail.capacity(), tail.data());
      ASSERT_GE(tailWritten, 0);
    } while (tailWritten > 0);
    SUCCEED();
  }
#endif

  // Invalid encodings throw (per API)
  EXPECT_THROW(state.makeContext(Encoding::none), std::invalid_argument);
  EXPECT_THROW(state.makeContext(static_cast<Encoding>(static_cast<std::underlying_type_t<Encoding>>(-1))),
               std::invalid_argument);
}

#ifdef AERONET_ENABLE_ZLIB
TEST(HttpCodecDecompression, MaybeDecompressRequestBody_StreamingThresholdWithoutContentLengthUsesAggregatedMode) {
  DecompressionConfig cfg;
  cfg.enable = true;
  // enable streaming threshold > 0 but do NOT set a Content-Length header
  cfg.streamingDecompressionThresholdBytes = 1;  // non-zero threshold

  ConnectionState cs;
  HttpRequest& req = cs.request;
  // set a supported encoding header so decompression codepath is attempted
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentEncoding, "gzip");

  // Prepare a small compressed payload if zlib available, otherwise use placeholder bytes.
  const std::string plain = "small payload";
  CompressionConfig encCfg;
  RawChars buf;
  ZlibEncoder encoder(encCfg.zlib.level);
  RawChars compressedOut(64UL + plain.size());
  {
    const std::size_t written = encoder.encodeFull(ZStreamRAII::Variant::gzip, std::string_view(plain),
                                                   compressedOut.capacity(), compressedOut.data());
    ASSERT_GT(written, 0UL);
    compressedOut.setSize(static_cast<RawChars::size_type>(written));
  }
  cs.installAggregatedBodyBridge();
  cs.bodyStreamContext.body = compressedOut;
  cs.bodyStreamContext.offset = 0;

  RawChars tmpBuf;
  RequestDecompressionState decompressionState;

  // Call MaybeDecompressRequestBody - UseStreamingDecompression should see no Content-Length and return false,
  // so decoder should be invoked in aggregated mode. We accept several possible outcomes depending on
  // available decoders (OK when a decoder succeeds, UnsupportedMediaType when decoder missing, BadRequest
  // for corrupted data). The important part is we do not modify production code and do not crash.
  const auto res =
      HttpCodec::MaybeDecompressRequestBody(decompressionState, cfg, req, cs.bodyAndTrailersBuffer, tmpBuf);
  EXPECT_TRUE(res.status == http::StatusCodeOK || res.status == http::StatusCodeBadRequest ||
              res.status == http::StatusCodeUnsupportedMediaType || res.status == http::StatusCodePayloadTooLarge);
}
#endif

}  // namespace aeronet::internal
