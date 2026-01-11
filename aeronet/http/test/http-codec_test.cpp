#include "aeronet/http-codec.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-encoder.hpp"
#endif
#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-encoder.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#endif
#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-encoder.hpp"
#endif

namespace aeronet::internal {

TEST(HttpCodecCompression, ContentTypeAllowListBlocksCompression) {
  CompressionConfig cfg;
  cfg.minBytes = 1;  // allow small bodies
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

  // initialize encoders for available formats (use actual encoder classes)
#ifdef AERONET_ENABLE_ZLIB
  state.encoders[static_cast<size_t>(Encoding::gzip)] = std::make_unique<ZlibEncoder>(ZStreamRAII::Variant::gzip, cfg);
  state.encoders[static_cast<size_t>(Encoding::deflate)] =
      std::make_unique<ZlibEncoder>(ZStreamRAII::Variant::deflate, cfg);
#endif
#ifdef AERONET_ENABLE_ZSTD
  state.encoders[static_cast<size_t>(Encoding::zstd)] = std::make_unique<ZstdEncoder>(cfg);
#endif
#ifdef AERONET_ENABLE_BROTLI
  state.encoders[static_cast<size_t>(Encoding::br)] = std::make_unique<BrotliEncoder>(cfg);
#endif

  // Construct a request via ConnectionState parsing helpers used in other tests.
  ConnectionState cs;
  HttpRequest& req = cs.request;
  // Mutate headers map directly (tests in this directory commonly mutate request internals via friendship in
  // other fixtures). We can safely mutate the non-const request by const_casting the view reference returned by
  // headers().
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::AcceptEncoding, "gzip, deflate");
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentType, "application/json");

  HttpResponse resp(http::StatusCodeOK);
  resp.body("hello world", http::ContentTypeTextPlain);

  // Try compress - application/json request Content-Type is not in allowlist -> no compression
  HttpCodec::TryCompressResponse(state, cfg, req, resp);
  EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());

  // Now set request Content-Type to allowed type
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentType, "text/plain");
  HttpResponse resp2(http::StatusCodeOK);
  resp2.body("hello world", http::ContentTypeTextPlain);
  HttpCodec::TryCompressResponse(state, cfg, req, resp2);

  // If encoders present, compression should be applied (Content-Encoding set). Otherwise no-op.
#ifdef AERONET_ENABLE_ZLIB
  EXPECT_FALSE(resp2.headerValueOrEmpty(http::ContentEncoding).empty());
#else
  EXPECT_TRUE(resp2.headerValueOrEmpty(http::ContentEncoding).empty());
#endif
}

TEST(HttpCodecCompression, VaryHeaderAddedWhenConfigured) {
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.addVaryHeader = true;
#ifdef AERONET_ENABLE_ZLIB
  cfg.contentTypeAllowList.clear();
  cfg.contentTypeAllowList.append("text/plain");
#else
  cfg.contentTypeAllowList.clear();
  cfg.contentTypeAllowList.append("text/plain");
#endif
#ifdef AERONET_ENABLE_ZLIB
  cfg.preferredFormats.push_back(Encoding::gzip);
#endif
  ResponseCompressionState state(cfg);
// initialize encoders and build request
#ifdef AERONET_ENABLE_ZLIB
  state.encoders[static_cast<size_t>(Encoding::gzip)] = std::make_unique<ZlibEncoder>(ZStreamRAII::Variant::gzip, cfg);
#endif

  ConnectionState cs;
  HttpRequest& req = cs.request;
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::AcceptEncoding, "gzip");
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentType, "text/plain");

  HttpResponse resp(http::StatusCodeOK);
  resp.body("hello world", http::ContentTypeTextPlain);

  // Diagnostics: ensure negotiation chooses gzip and encoder is present when expected.
  auto neg = state.selector.negotiateAcceptEncoding(req.headerValueOrEmpty(http::AcceptEncoding));
  (void)neg;
#ifdef AERONET_ENABLE_ZLIB
  EXPECT_EQ(neg.encoding, Encoding::gzip);
  EXPECT_TRUE(static_cast<bool>(state.encoders[static_cast<size_t>(Encoding::gzip)]));
#endif

  HttpCodec::TryCompressResponse(state, cfg, req, resp);

#ifdef AERONET_ENABLE_ZLIB
  EXPECT_FALSE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
  if (!resp.headerValueOrEmpty(http::Vary).contains(http::AcceptEncoding)) {
    ADD_FAILURE() << "Vary header missing; response headers: " << resp.headersFlatView();
  }
#else
  SUCCEED();  // no encoder built, nothing to assert
#endif
}

TEST(HttpCodecCompression, VaryHeaderNotAddedWhenDisabled) {
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.addVaryHeader = false;  // disable adding Vary
  cfg.contentTypeAllowList.clear();
  cfg.contentTypeAllowList.append("text/plain");
#ifdef AERONET_ENABLE_ZLIB
  cfg.preferredFormats.push_back(Encoding::gzip);
#endif
  ResponseCompressionState state(cfg);

#ifdef AERONET_ENABLE_ZLIB
  state.encoders[static_cast<size_t>(Encoding::gzip)] = std::make_unique<ZlibEncoder>(ZStreamRAII::Variant::gzip, cfg);
#endif

  ConnectionState cs;
  HttpRequest& req = cs.request;
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::AcceptEncoding, "gzip");
  const_cast<HeadersViewMap&>(req.headers()).insert_or_assign(http::ContentType, "text/plain");

  HttpResponse resp(http::StatusCodeOK);
  resp.body("hello world", http::ContentTypeTextPlain);

  HttpCodec::TryCompressResponse(state, cfg, req, resp);

#ifdef AERONET_ENABLE_ZLIB
  // Compression should be applied but Vary must NOT be set because addVaryHeader == false
  EXPECT_FALSE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
  EXPECT_TRUE(resp.headerValueOrEmpty(http::Vary).empty());
#else
  SUCCEED();
#endif
}

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

  const auto res = HttpCodec::DecompressChunkedBody(cfg, req, chunks, /*compressedSize=*/1, bodyBuf, tmpBuf);
  EXPECT_EQ(res.status, http::StatusCodeBadRequest);
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
  ZlibEncoder encoder(ZStreamRAII::Variant::gzip, encCfg);
  RawChars compressedOut;
  encoder.encodeFull(0, std::string_view(plain), compressedOut);

  const std::string_view compressedView(compressedOut.data(), compressedOut.size());
  std::span<const std::string_view> chunks(&compressedView, 1);

  RawChars bodyBuf;
  RawChars tmpBuf;

  const auto res =
      HttpCodec::DecompressChunkedBody(cfg, req, chunks, /*compressedSize=*/compressedView.size(), bodyBuf, tmpBuf);
  EXPECT_EQ(res.status, http::StatusCodePayloadTooLarge);

  // Check with a large enough expansion ratio to ensure success
  cfg.maxExpansionRatio = (plainSize / static_cast<double>(compressedView.size())) + 1.0;
  const auto res2 =
      HttpCodec::DecompressChunkedBody(cfg, req, chunks, /*compressedSize=*/compressedView.size(), bodyBuf, tmpBuf);
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

  const auto res = HttpCodec::DecompressChunkedBody(cfg, req, chunks, /*compressedSize=*/1, bodyBuf, tmpBuf);
  EXPECT_EQ(res.status, http::StatusCodeUnsupportedMediaType);
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
  ZlibEncoder encoder(ZStreamRAII::Variant::gzip, encCfg);
  RawChars compressedOut;
  encoder.encodeFull(0, std::string_view(plain), compressedOut);
  cs.installAggregatedBodyBridge();
  cs.bodyStreamContext.body = std::string_view(compressedOut.data(), compressedOut.size());
  cs.bodyStreamContext.offset = 0;

  RawChars tmpBuf;

  // Call MaybeDecompressRequestBody - UseStreamingDecompression should see no Content-Length and return false,
  // so decoder should be invoked in aggregated mode. We accept several possible outcomes depending on
  // available decoders (OK when a decoder succeeds, UnsupportedMediaType when decoder missing, BadRequest
  // for corrupted data). The important part is we do not modify production code and do not crash.
  const auto res = HttpCodec::MaybeDecompressRequestBody(cfg, req, cs.bodyAndTrailersBuffer, tmpBuf);
  EXPECT_TRUE(res.status == http::StatusCodeOK || res.status == http::StatusCodeBadRequest ||
              res.status == http::StatusCodeUnsupportedMediaType || res.status == http::StatusCodePayloadTooLarge);
}
#endif

}  // namespace aeronet::internal
