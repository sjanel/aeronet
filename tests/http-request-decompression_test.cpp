#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/toupperlower.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-encoder.hpp"
#include "brotli/encode.h"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-encoder.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-encoder.hpp"
#endif

namespace aeronet {

namespace {

RawChars buf;

std::mt19937 rng(12345);
std::uniform_int_distribution<std::size_t> nbSpaces(0, 3);
std::uniform_int_distribution<int> toUpper(0, 1);

std::string ContentEncodingConcat(std::span<const std::string_view> encodings) {
  std::string contentEncodingValue;

  for (std::string_view encoding : encodings) {
    if (!contentEncodingValue.empty()) {
      contentEncodingValue.append(",");
      contentEncodingValue.append(nbSpaces(rng), '\t');
    }
    contentEncodingValue.append(nbSpaces(rng), ' ');
    contentEncodingValue.append(encoding);
    contentEncodingValue.append(nbSpaces(rng), ' ');
  }

  return contentEncodingValue;
}

// Not constexpr because it could be empty depending on build flags
const std::vector<std::string_view> kKnownEncodings = []() {
  std::vector<std::string_view> vec;
#ifdef AERONET_ENABLE_ZLIB
  vec.push_back(http::gzip);
  vec.push_back(http::deflate);
#endif
#ifdef AERONET_ENABLE_ZSTD
  vec.push_back(http::zstd);
#endif
#ifdef AERONET_ENABLE_BROTLI
  vec.push_back(http::br);
#endif
  return vec;
}();

constexpr std::string_view kKnownEncodingsWithIdentity[] = {
    http::identity,
#ifdef AERONET_ENABLE_ZLIB
    http::gzip,     http::deflate,
#endif
#ifdef AERONET_ENABLE_ZSTD
    http::zstd,
#endif
#ifdef AERONET_ENABLE_BROTLI
    http::br,
#endif
};

RawChars compress(std::string_view alg, std::string_view input) {
  RawChars buf;
  CompressionConfig cc;
  if (CaseInsensitiveEqual(alg, "identity")) {
    buf.assign(input);
#ifdef AERONET_ENABLE_ZLIB
    // NOLINTNEXTLINE(readability-else-after-return)
  } else if (CaseInsensitiveEqual(alg, "gzip")) {
    ZlibEncoder encoder(cc.zlib.level);
    buf.reserve(64UL + ZDeflateBound(nullptr, input.size()));
    const std::size_t written = encoder.encodeFull(ZStreamRAII::Variant::gzip, input, buf.capacity(), buf.data());
    if (written == 0) {
      throw std::runtime_error("gzip compression failed");
    }
    buf.setSize(written);
  } else if (CaseInsensitiveEqual(alg, "deflate")) {
    ZlibEncoder encoder(cc.zlib.level);
    buf.reserve(64UL + ZDeflateBound(nullptr, input.size()));
    const std::size_t written = encoder.encodeFull(ZStreamRAII::Variant::deflate, input, buf.capacity(), buf.data());
    if (written == 0) {
      throw std::runtime_error("deflate compression failed");
    }
    buf.setSize(written);
#endif
#ifdef AERONET_ENABLE_ZSTD
  } else if (CaseInsensitiveEqual(alg, "zstd")) {
    ZstdEncoder encoder(cc.zstd);
    buf.reserve(ZSTD_compressBound(input.size()));
    const std::size_t written = encoder.encodeFull(input, buf.capacity(), buf.data());
    if (written == 0) {
      throw std::runtime_error("zstd compression failed");
    }
    buf.setSize(written);
#endif
#ifdef AERONET_ENABLE_BROTLI
  } else if (CaseInsensitiveEqual(alg, "br")) {
    BrotliEncoder encoder(cc.brotli);
    buf.reserve(BrotliEncoderMaxCompressedSize(input.size()));
    const std::size_t written = encoder.encodeFull(input, buf.capacity(), buf.data());
    if (written == 0) {
      throw std::runtime_error("brotli compression failed");
    }
    buf.setSize(written);
#endif
  } else {
    // Unsupported algorithm, do not compress
  }
  return buf;
}

struct ClientRawResponse {
  int status{};
  std::string body;
  std::string headersRaw;
};

ClientRawResponse rawPost(uint16_t port, std::string_view target,
                          std::vector<std::pair<std::string_view, std::string_view>> headers, std::string_view body) {
  test::RequestOptions opt;
  opt.method = http::POST;
  opt.target = target;
  opt.connection = http::close;
  opt.body = body;
  opt.headers = std::move(headers);
  auto raw = test::request(port, opt);
  if (!raw) {
    throw std::runtime_error("request failed");
  }
  ClientRawResponse resp;
  auto& str = *raw;
  if (str.empty()) {
    throw std::runtime_error("empty response from server");
  }
  auto firstSpace = str.find(' ');
  if (firstSpace == std::string::npos) {
    throw std::runtime_error(std::string("malformed status line in response: ") + str);
  }
  auto secondSpace = str.find(' ', firstSpace + 1);
  if (secondSpace == std::string::npos) {
    throw std::runtime_error(std::string("malformed status line in response: ") + str);
  }
  resp.status = read3(str.substr(firstSpace + 1, secondSpace - firstSpace - 1).data());
  auto headersEnd = str.find(http::DoubleCRLF);
  if (headersEnd == std::string::npos) {
    throw std::runtime_error(std::string("missing header/body separator (CRLFCRLF) in response: ") + str);
  }
  resp.headersRaw = str.substr(0, headersEnd);
  resp.body = str.substr(headersEnd + http::DoubleCRLF.size());
  return resp;
}

test::TestServer ts(HttpServerConfig{});

}  // namespace

TEST(HttpRequestDecompression, SingleSmallPayload) {
  std::string_view plain = "HelloCompressedWorld";

  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.postRouterUpdate([](Router& router) {
    router.setDefault([](const HttpRequest& req) {
      EXPECT_TRUE(req.headerValue(http::ContentEncoding) == http::identity ||
                  !req.headerValue(http::ContentEncoding).has_value());
      return HttpResponse(req.body());
    });
  });

  for (std::string_view encoding : kKnownEncodingsWithIdentity) {
    auto comp = compress(encoding, plain);
    auto resp = rawPost(ts.port(), "/single", {{"Content-Encoding", encoding}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK) << "Failed for encoding: " << encoding;
    EXPECT_EQ(resp.body, plain) << "Failed for encoding: " << encoding;
  }
}

TEST(HttpRequestDecompression, SingleNoContentEncoding) {
  std::string_view plain = "HelloCompressedWorld";

  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.postRouterUpdate(
      [](Router& router) { router.setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); }); });

  for (std::string_view encoding : kKnownEncodingsWithIdentity) {
    auto comp = compress(encoding, plain);
    auto resp = rawPost(ts.port(), "/single", {}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
    EXPECT_EQ(resp.body, std::string_view(comp));
  }
}

TEST(HttpRequestDecompression, SingleLargePayloadWithHeadersCheck) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  const std::string plain(10000, 'A');

  for (std::string_view encoding : kKnownEncodings) {
    const auto comp = compress(encoding, plain);
    const std::size_t compressedSize = comp.size();

    ts.resetRouterAndGet().setDefault([plain, compressedSize, encoding](const HttpRequest& req) {
      EXPECT_EQ(req.body(), plain);
      EXPECT_FALSE(req.headerValue(http::ContentEncoding));
      EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodingHeaderName), encoding);
      EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodedLengthHeaderName), std::to_string(compressedSize));

      EXPECT_EQ(req.body().size(), StringToIntegral<std::size_t>(req.headerValueOrEmpty(http::ContentLength)));

      return HttpResponse("Z");
    });
    auto resp = rawPost(ts.port(), "/d", {{"Content-Encoding", encoding}}, comp);
    EXPECT_EQ(resp.status, 200);
  }
}

TEST(HttpRequestDecompression, DualCompressionWithSpaces) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain(1000, 'A');
  std::ranges::iota(plain.begin(), plain.end(), 'A');

  // Loop on all pairs of known encodings (there will be duplicates, but it's ok, we test that also)
  for (std::string_view firstEnc : kKnownEncodingsWithIdentity) {
    for (std::string_view secondEnc : kKnownEncodingsWithIdentity) {
      const auto firstComp = compress(firstEnc, plain);
      const auto dualComp = compress(secondEnc, firstComp);

      const bool testHeaders = firstEnc != http::identity || secondEnc != http::identity;

      std::string contentEncodingValue = ContentEncodingConcat(std::vector<std::string_view>{firstEnc, secondEnc});

      ts.resetRouterAndGet().setDefault(
          [testHeaders, contentEncodingTrimmed = TrimOws(contentEncodingValue)](const HttpRequest& req) {
            if (testHeaders) {
              EXPECT_FALSE(req.headerValue(http::ContentEncoding).has_value());
              EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodingHeaderName), contentEncodingTrimmed);
            }
            return HttpResponse(req.body());
          });
      auto resp = rawPost(ts.port(), "/dd", {{"Content-Encoding", contentEncodingValue}}, dualComp);
      ASSERT_EQ(resp.status, 200) << "Failed for encoding chain: " << contentEncodingValue;
      EXPECT_EQ(resp.body, plain) << "Failed for encoding chain: " << contentEncodingValue;
    }
  }
}

TEST(HttpRequestDecompression, TripleCompressionWithSpaces) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain(1000, 'A');
  std::ranges::iota(plain.begin(), plain.end(), 'A');

  std::mt19937 rng(12345);
  std::uniform_int_distribution<std::size_t> nbSpaces(0, 3);

  // Loop on all tuples of known encodings (there will be duplicates, but it's ok, we test that also)
  for (std::string_view firstEnc : kKnownEncodingsWithIdentity) {
    for (std::string_view secondEnc : kKnownEncodingsWithIdentity) {
      for (std::string_view thirdEnc : kKnownEncodingsWithIdentity) {
        const auto firstComp = compress(firstEnc, plain);
        const auto dualComp = compress(secondEnc, firstComp);
        const auto tripleComp = compress(thirdEnc, dualComp);

        const bool testHeaders =
            firstEnc != http::identity || secondEnc != http::identity || thirdEnc != http::identity;

        std::string contentEncodingValue =
            ContentEncodingConcat(std::vector<std::string_view>{firstEnc, secondEnc, thirdEnc});

        ts.resetRouterAndGet().setDefault(
            [testHeaders, contentEncodingTrimmed = TrimOws(contentEncodingValue)](const HttpRequest& req) {
              if (testHeaders) {
                EXPECT_FALSE(req.headerValue(http::ContentEncoding).has_value());
                EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodingHeaderName), contentEncodingTrimmed);
              }
              return HttpResponse(req.body());
            });
        auto resp = rawPost(ts.port(), "/dd", {{"Content-Encoding", contentEncodingValue}}, tripleComp);
        ASSERT_EQ(resp.status, 200) << "Failed for encoding chain: " << contentEncodingValue;
        EXPECT_EQ(resp.body, plain) << "Failed for encoding chain: " << contentEncodingValue;
      }
    }
  }
}

TEST(HttpRequestDecompression, SingleUnknownCodingRejected) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.resetRouterAndGet().setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse("U"); });
  std::string body = "abc";  // not used
  auto resp = rawPost(ts.port(), "/u", {{"Content-Encoding", "snappy"}}, body);
#if defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZSTD)
  EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
#else
  EXPECT_EQ(resp.status, http::StatusCodeOK);  // decompression disabled, pass-through
#endif
}

TEST(HttpRequestDecompression, UnknownCodingRejectedInChain) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.resetRouterAndGet().setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse("U"); });
  std::string body = "abc";
  for (std::string_view encoding : kKnownEncodings) {
    auto compressed = compress(encoding, body);
    std::string contentEncodingValue("snappy, ");
    contentEncodingValue.append(encoding);
    auto resp = rawPost(ts.port(), "/u", {{"Content-Encoding", contentEncodingValue}}, compressed);
    EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
    contentEncodingValue.append(", identity");
    resp = rawPost(ts.port(), "/u", {{"Content-Encoding", contentEncodingValue}}, compressed);
    EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
  }
}

TEST(HttpRequestDecompression, EmptyTokenRejected) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string body = "xyz";
#if defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZSTD)
  static constexpr http::StatusCode kExpectedStatus = http::StatusCodeBadRequest;
#else
  static constexpr http::StatusCode kExpectedStatus = http::StatusCodeOK;  // decompression disabled, pass-through
#endif
  auto resp = rawPost(ts.port(), "/e", {{"Content-Encoding", "identity,,identity"}}, body);
  EXPECT_EQ(resp.status, kExpectedStatus);
  resp = rawPost(ts.port(), "/e", {{"Content-Encoding", "identity,,"}}, body);
  EXPECT_EQ(resp.status, kExpectedStatus);
  resp = rawPost(ts.port(), "/e", {{"Content-Encoding", ","}}, body);
  EXPECT_EQ(resp.status, kExpectedStatus);
  resp = rawPost(ts.port(), "/e", {{"Content-Encoding", ""}}, body);
  EXPECT_EQ(resp.status, kExpectedStatus);
}

TEST(HttpRequestDecompression, DisabledFeaturePassThrough) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.enable = false;
  });
  const std::string plain(100, 'A');

  for (std::string_view encoding : kKnownEncodings) {
    const auto comp = compress(encoding, plain);

    ts.resetRouterAndGet().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
    auto resp = rawPost(ts.port(), "/d", {{"Content-Encoding", encoding}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);

    EXPECT_EQ(resp.body, std::string_view(comp));
  }
}

TEST(HttpRequestDecompression, MaxCompressedBytesExceededEarlyReturn) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });

  // Any non-empty Content-Encoding header will cause the decompression path to be considered.
  // We send a body larger than `maxCompressedBytes` to hit the early PayloadTooLarge return.
  std::string plain = "abcdefghijkl";

  for (std::string_view encoding : kKnownEncodings) {
    auto comp = compress(encoding, plain);
    ts.postConfigUpdate([&comp](HttpServerConfig& cfg) { cfg.decompression.maxCompressedBytes = comp.size() - 1; });
    auto resp = rawPost(ts.port(), "/too_big", {{"Content-Encoding", encoding}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodePayloadTooLarge);

    ts.postConfigUpdate([&comp](HttpServerConfig& cfg) { cfg.decompression.maxCompressedBytes = comp.size(); });
    resp = rawPost(ts.port(), "/nowok", {{"Content-Encoding", encoding}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
    EXPECT_EQ(resp.body, plain);
  }
}

TEST(HttpRequestDecompression, ExpansionRatioGuard) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.maxExpansionRatio = 2.0;
    cfg.decompression.maxDecompressedBytes = 100000;
  });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  // Highly compressible large input -> gzip then send; expect rejection if ratio >2
  std::string large(100000, 'A');

  for (std::string_view encoding : kKnownEncodings) {
    auto comp = compress(encoding, large);
    // Ensure it actually compresses well
    ASSERT_LT(comp.size() * 2, large.size());
    auto resp = rawPost(ts.port(), "/rg", {{"Content-Encoding", encoding}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodePayloadTooLarge);

    // Now send with a bigger maxExpansionRatio and expect success
    const double actualExpansionRatio = static_cast<double>(large.size()) / static_cast<double>(comp.size());
    ts.postConfigUpdate([actualExpansionRatio](HttpServerConfig& cfg) {
      cfg.decompression.maxExpansionRatio = actualExpansionRatio + 1;
    });
    resp = rawPost(ts.port(), "/rg_ok", {{"Content-Encoding", encoding}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
    EXPECT_EQ(resp.body, large);
  }
}

TEST(HttpRequestDecompression, StreamingThresholdLargeBody) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.decoderChunkSize = 16;
  });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });

  std::string plain(4096, 'S');

  for (std::string_view encoding : kKnownEncodings) {
    auto comp = compress(encoding, plain);
    ts.postConfigUpdate(
        [&comp](HttpServerConfig& cfg) { cfg.decompression.streamingDecompressionThresholdBytes = comp.size(); });
    auto resp = rawPost(ts.port(), "/stream", {{"Content-Encoding", encoding}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);

    // disable streaming
    ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression.streamingDecompressionThresholdBytes = 0; });

    resp = rawPost(ts.port(), "/stream", {{"Content-Encoding", encoding}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);

    // disable streaming based on size smaller than threshold
    ts.postConfigUpdate(
        [&comp](HttpServerConfig& cfg) { cfg.decompression.streamingDecompressionThresholdBytes = comp.size() + 1; });

    resp = rawPost(ts.port(), "/stream", {{"Content-Encoding", encoding}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
  }
}

namespace {
void ExpectTrailers(std::vector<std::string_view> encodings, bool insertBadTrailer = false, bool corruptData = false) {
  std::string plain(10000, 'L');
  std::ranges::iota(plain, 'A');

  ts.router().setDefault([&plain](const HttpRequest& req) {
    EXPECT_EQ(req.body(), plain);
    // Expect two trailers preserved
    EXPECT_EQ(req.trailers().size(), 2U);
    auto it = req.trailers().find("X-Checksum");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      EXPECT_EQ(it->second, "abc123");
    }
    auto it2 = req.trailers().find("X-Note");
    EXPECT_NE(it2, req.trailers().end());
    if (it2 != req.trailers().end()) {
      EXPECT_EQ(it2->second, "final");
    }
    return HttpResponse("CompressLargeBodyWithTrailers OK");
  });

  std::string contentEncodingValue = ContentEncodingConcat(encodings);

  RawChars comp(plain);
  for (std::string_view encoding : encodings) {
    comp = compress(encoding, comp);
    if (corruptData && encoding == encodings.back() && encoding != http::identity) {
      test::CorruptData(encoding, comp);
    }
  }

  std::ostringstream hdr;
  hdr << "POST /trail_compress_large HTTP/1.1\r\n";
  hdr << "Host: example.com\r\n";
  hdr << "Transfer-Encoding: chunked\r\n";
  hdr << "Content-Encoding: " << contentEncodingValue << http::CRLF;
  hdr << "Connection: close\r\n";
  hdr << http::CRLF;
  // chunk size in hex
  hdr << std::hex << comp.size() << http::CRLF;

  std::string req = hdr.str();
  req.append(comp);
  req += "\r\n0\r\n";
  req += MakeHttp1HeaderLine("X-Checksum", "abc123");
  if (insertBadTrailer) {
    // Insert a malformed trailer (no colon)
    req += "Bad-Trailer-Entry\r\n";
  }
  req += MakeHttp1HeaderLine("X-Note", "final");
  req += http::CRLF;

  test::ClientConnection sock(ts.port());
  int fd = sock.fd();

  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);

  if (insertBadTrailer || corruptData) {
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 400"))
        << "Failed for encoding: " << contentEncodingValue << ", response: " << resp;
  } else {
    const auto maxCompressedBytes = ts.server.config().decompression.maxCompressedBytes;
    const auto maxBodyBytes = ts.server.config().maxBodyBytes;
    if (comp.size() > maxBodyBytes || (maxCompressedBytes != 0 && comp.size() > maxCompressedBytes)) {
      EXPECT_TRUE(resp.starts_with("HTTP/1.1 413"))
          << "Failed for encoding: " << contentEncodingValue << ", response: " << resp;
    } else {
      EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"))
          << "Failed for encoding: " << contentEncodingValue << ", response: " << resp;
      EXPECT_TRUE(resp.contains("\r\n\r\nCompressLargeBodyWithTrailers OK"))
          << "Failed for encoding: " << contentEncodingValue << ", response: " << resp;
    }
  }
}
}  // namespace

TEST(HttpRequestDecompression, SingleCompressLargeBodyWithBadTrailers) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  for (std::string_view encoding : kKnownEncodings) {
    ExpectTrailers({encoding}, true);
  }
}

TEST(HttpRequestDecompression, SingleCompressLargeBodyWithTrailers) {
  static constexpr std::size_t kMaxCompressedBytes[]{0, 128, 1024};
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  for (std::size_t maxCompressedBytes : kMaxCompressedBytes) {
    ts.postConfigUpdate(
        [maxCompressedBytes](HttpServerConfig& cfg) { cfg.decompression.maxCompressedBytes = maxCompressedBytes; });
    for (std::string_view encoding : kKnownEncodings) {
      ExpectTrailers({encoding});
    }
  }
}

TEST(HttpRequestDecompression, DualCompressLargeBodyWithTrailers) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  for (bool corruptData : {false, true}) {
    for (std::string_view first : kKnownEncodings) {
      for (std::string_view second : kKnownEncodings) {
        ExpectTrailers({first, second}, false, corruptData);
      }
    }
  }
}

TEST(HttpRequestDecompression, TripleCompressLargeBodyWithTrailers) {
  static constexpr std::size_t kMaxBodyBytes[]{1, 128, 1024};
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  for (std::size_t maxBodyBytes : kMaxBodyBytes) {
    ts.postConfigUpdate([maxBodyBytes](HttpServerConfig& cfg) { cfg.maxBodyBytes = maxBodyBytes; });
    for (std::string_view first : kKnownEncodings) {
      for (std::string_view second : kKnownEncodings) {
        for (std::string_view third : kKnownEncodings) {
          ExpectTrailers({first, second, third});
        }
      }
    }
  }
}

TEST(HttpRequestDecompression, MixedCaseTokens) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = "CaseCheck";
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });

  for (std::string_view encoding : kKnownEncodings) {
    std::string mixedCaseEnc(encoding);
    for (char& ch : mixedCaseEnc) {
      if (toUpper(rng) == 1) {
        ch = toupper(ch);
      }
    }
    auto comp = compress(encoding, plain);
    auto resp = rawPost(ts.port(), "/case", {{"Content-Encoding", mixedCaseEnc}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
    EXPECT_EQ(resp.body, plain);
  }
}

// ---------------- Corruption / truncated frame tests ----------------

namespace {

constexpr std::size_t kStreamingThresholds[]{0, 1};

}

TEST(HttpRequestDecompression, CorruptedCompressedData) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = std::string(200, 'G');
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });

  for (std::size_t threshold : kStreamingThresholds) {
    ts.postConfigUpdate(
        [threshold](HttpServerConfig& cfg) { cfg.decompression.streamingDecompressionThresholdBytes = threshold; });
    for (std::string_view encoding : kKnownEncodings) {
      auto comp = compress(encoding, plain);
      if (encoding == http::identity) {
        // identity compression cannot be corrupted
        continue;
      }
      test::CorruptData(encoding, comp);

      auto resp = rawPost(ts.port(), "/corrupt", {{"Content-Encoding", encoding}}, comp);
      EXPECT_EQ(resp.status, http::StatusCodeBadRequest) << "Expected 400 for corrupted encoding: " << encoding;
    }
  }
}

}  // namespace aeronet