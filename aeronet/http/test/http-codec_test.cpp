#include "aeronet/http-codec.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>

#include "aeronet/compression-config.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"

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
  if (!resp.headerValueOrEmpty(http::Vary).contains("accept-encoding")) {
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
}  // namespace aeronet::internal
