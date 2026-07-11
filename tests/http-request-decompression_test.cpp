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

#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/toupperlower.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

namespace {

RawChars buf;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization,bugprone-random-generator-seed)
std::mt19937 rng(12345);

std::string ContentEncodingConcat(std::span<const Encoding> encodings) {
  std::string contentEncodingValue;

  std::uniform_int_distribution<std::size_t> nbSpaces(0, 3);
  for (Encoding encoding : encodings) {
    if (!contentEncodingValue.empty()) {
      contentEncodingValue.append(",");
      contentEncodingValue.append(nbSpaces(rng), '\t');
    }
    contentEncodingValue.append(nbSpaces(rng), ' ');
    contentEncodingValue.append(GetEncodingStr(encoding));
    contentEncodingValue.append(nbSpaces(rng), ' ');
  }

  return contentEncodingValue;
}

struct ClientRawResponse {
  int status{};
  std::string body;
  std::string headersRaw;
};

ClientRawResponse RawPost(uint16_t port, std::string_view target,
                          vector<std::pair<std::string_view, std::string_view>> headers, std::string_view body) {
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

test::TestServer ts;

}  // namespace

TEST(HttpRequestDecompression, SingleSmallPayload) {
  std::string_view plain = "HelloCompressedWorld";

  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.postRouterUpdate([](Router& router) {
    router.setDefault([](const HttpRequestView& req) {
      EXPECT_TRUE(req.headerValue(http::ContentEncoding) == http::identity ||
                  !req.headerValue(http::ContentEncoding).has_value());
      return HttpResponse(req.body());
    });
  });

  for (Encoding encoding : test::SupportedEncodingWithIdentity()) {
    auto comp = test::Compress(encoding, plain);
    auto resp = RawPost(ts.port(), "/single", {{"Content-Encoding", GetEncodingStr(encoding)}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK) << "Failed for encoding: " << GetEncodingStr(encoding);
    EXPECT_EQ(resp.body, plain) << "Failed for encoding: " << GetEncodingStr(encoding);
  }
}

TEST(HttpRequestDecompression, SingleNoContentEncoding) {
  std::string_view plain = "HelloCompressedWorld";

  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.postRouterUpdate(
      [](Router& router) { router.setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); }); });

  for (Encoding encoding : test::SupportedEncodingWithIdentity()) {
    auto comp = test::Compress(encoding, plain);
    auto resp = RawPost(ts.port(), "/single", {}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
    EXPECT_EQ(resp.body, std::string_view(comp));
  }
}

TEST(HttpRequestDecompression, SingleLargePayloadWithHeadersCheck) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  const std::string plain(10000, 'A');

  for (const Encoding enc : test::SupportedEncodings()) {
    const auto comp = test::Compress(enc, plain);
    const std::size_t compressedSize = comp.size();

    ts.resetRouterAndGet().setDefault([plain, compressedSize, enc](const HttpRequestView& req) {
      EXPECT_EQ(req.body(), plain);
      EXPECT_FALSE(req.headerValue(http::ContentEncoding));
      EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodingHeaderName), GetEncodingStr(enc));
      EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodedLengthHeaderName), std::to_string(compressedSize));

      EXPECT_EQ(req.body().size(), StringToIntegral<std::size_t>(req.headerValueOrEmpty(http::ContentLength)));

      return HttpResponse("Z");
    });
    auto resp = RawPost(ts.port(), "/d", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
    EXPECT_EQ(resp.status, 200);
  }
}

TEST(HttpRequestDecompression, DualCompressionWithSpaces) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain(1000, 'A');
  std::ranges::iota(plain.begin(), plain.end(), 'A');

  // Loop on all pairs of known encodings (there will be duplicates, but it's ok, we test that also)
  for (const Encoding firstEnc : test::SupportedEncodingWithIdentity()) {
    for (const Encoding secondEnc : test::SupportedEncodingWithIdentity()) {
      const auto firstComp = test::Compress(firstEnc, plain);
      const auto dualComp = test::Compress(secondEnc, firstComp);

      const bool testHeaders = firstEnc != Encoding::none || secondEnc != Encoding::none;

      std::string contentEncodingValue = ContentEncodingConcat(vector<Encoding>{firstEnc, secondEnc});

      ts.resetRouterAndGet().setDefault(
          [testHeaders, contentEncodingTrimmed = TrimOws(contentEncodingValue)](const HttpRequestView& req) {
            if (testHeaders) {
              EXPECT_FALSE(req.headerValue(http::ContentEncoding).has_value());
              EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodingHeaderName), contentEncodingTrimmed);
            }
            return HttpResponse(req.body());
          });
      auto resp = RawPost(ts.port(), "/dd", {{"Content-Encoding", contentEncodingValue}}, dualComp);
      ASSERT_EQ(resp.status, 200) << "Failed for encoding chain: " << contentEncodingValue;
      EXPECT_EQ(resp.body, plain) << "Failed for encoding chain: " << contentEncodingValue;
    }
  }
}

TEST(HttpRequestDecompression, TripleCompressionWithSpaces) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain(1000, 'A');
  std::ranges::iota(plain.begin(), plain.end(), 'A');

  // Loop on all tuples of known encodings (there will be duplicates, but it's ok, we test that also)
  for (const Encoding firstEnc : test::SupportedEncodingWithIdentity()) {
    for (const Encoding secondEnc : test::SupportedEncodingWithIdentity()) {
      for (const Encoding thirdEnc : test::SupportedEncodingWithIdentity()) {
        const auto firstComp = test::Compress(firstEnc, plain);
        const auto dualComp = test::Compress(secondEnc, firstComp);
        const auto tripleComp = test::Compress(thirdEnc, dualComp);

        const bool testHeaders =
            firstEnc != Encoding::none || secondEnc != Encoding::none || thirdEnc != Encoding::none;

        std::string contentEncodingValue = ContentEncodingConcat(vector<Encoding>{firstEnc, secondEnc, thirdEnc});

        ts.resetRouterAndGet().setDefault(
            [testHeaders, contentEncodingTrimmed = TrimOws(contentEncodingValue)](const HttpRequestView& req) {
              if (testHeaders) {
                EXPECT_FALSE(req.headerValue(http::ContentEncoding).has_value());
                EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodingHeaderName), contentEncodingTrimmed);
              }
              return HttpResponse(req.body());
            });
        auto resp = RawPost(ts.port(), "/dd", {{"Content-Encoding", contentEncodingValue}}, tripleComp);
        ASSERT_EQ(resp.status, 200) << "Failed for encoding chain: " << contentEncodingValue;
        EXPECT_EQ(resp.body, plain) << "Failed for encoding chain: " << contentEncodingValue;
      }
    }
  }
}

TEST(HttpRequestDecompression, SingleUnknownCodingRejected) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.resetRouterAndGet().setDefault([]([[maybe_unused]] const HttpRequestView& req) { return HttpResponse("U"); });
  std::string body = "abc";  // not used
  auto resp = RawPost(ts.port(), "/u", {{"Content-Encoding", "snappy"}}, body);
#if defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZSTD)
  EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
#else
  EXPECT_EQ(resp.status, http::StatusCodeOK);  // decompression disabled, pass-through
#endif
}

TEST(HttpRequestDecompression, UnknownCodingRejectedInChain) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.resetRouterAndGet().setDefault([]([[maybe_unused]] const HttpRequestView& req) { return HttpResponse("U"); });
  std::string body = "abc";
  for (const Encoding enc : test::SupportedEncodings()) {
    auto compressed = test::Compress(enc, body);
    std::string contentEncodingValue("snappy, ");
    contentEncodingValue.append(GetEncodingStr(enc));
    auto resp = RawPost(ts.port(), "/u", {{"Content-Encoding", contentEncodingValue}}, compressed);
    EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
    contentEncodingValue.append(", identity");
    resp = RawPost(ts.port(), "/u", {{"Content-Encoding", contentEncodingValue}}, compressed);
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
  auto resp = RawPost(ts.port(), "/e", {{"Content-Encoding", "identity,,identity"}}, body);
  EXPECT_EQ(resp.status, kExpectedStatus);
  resp = RawPost(ts.port(), "/e", {{"Content-Encoding", "identity,,"}}, body);
  EXPECT_EQ(resp.status, kExpectedStatus);
  resp = RawPost(ts.port(), "/e", {{"Content-Encoding", ","}}, body);
  EXPECT_EQ(resp.status, kExpectedStatus);
  resp = RawPost(ts.port(), "/e", {{"Content-Encoding", ""}}, body);
  EXPECT_EQ(resp.status, kExpectedStatus);
}

TEST(HttpRequestDecompression, DisabledFeaturePassThrough) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.enable = false;
  });
  const std::string plain(100, 'A');

  for (const Encoding enc : test::SupportedEncodings()) {
    const auto comp = test::Compress(enc, plain);

    ts.resetRouterAndGet().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });
    auto resp = RawPost(ts.port(), "/d", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);

    EXPECT_EQ(resp.body, std::string_view(comp));
  }
}

TEST(HttpRequestDecompression, MaxCompressedBytesExceededEarlyReturn) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });

  // Any non-empty Content-Encoding header will cause the decompression path to be considered.
  // We send a body larger than `maxCompressedBytes` to hit the early PayloadTooLarge return.
  std::string plain = "abcdefghijkl";

  for (const Encoding enc : test::SupportedEncodings()) {
    auto comp = test::Compress(enc, plain);
    ts.postConfigUpdate([&comp](HttpServerConfig& cfg) { cfg.decompression.maxCompressedBytes = comp.size() - 1; });
    auto resp = RawPost(ts.port(), "/too_big", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodePayloadTooLarge);

    ts.postConfigUpdate([&comp](HttpServerConfig& cfg) { cfg.decompression.maxCompressedBytes = comp.size(); });
    resp = RawPost(ts.port(), "/nowok", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
    EXPECT_EQ(resp.body, plain);
  }
}

TEST(HttpRequestDecompression, ChunkedCompressedBodyExceedsMaxBodyBytesCumulatively) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.maxCompressedBytes = 0;
  });
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });

  const auto defaultMaxBodyBytes = ts.server.config().maxBodyBytes;
  std::string plain(4096, 'Q');

  for (const Encoding enc : test::SupportedEncodings()) {
    const auto comp = test::Compress(enc, plain);
    ASSERT_GT(comp.size(), 1U) << "Compressed payload unexpectedly too small for encoding: " << GetEncodingStr(enc);

    const std::size_t firstChunkSize = comp.size() - 1;
    const std::size_t secondChunkSize = 1;
    ASSERT_LE(firstChunkSize, comp.size());

    ts.postConfigUpdate([firstChunkSize](HttpServerConfig& cfg) { cfg.maxBodyBytes = firstChunkSize; });

    std::ostringstream hdr;
    hdr << "POST /chunked-cumulative-limit HTTP/1.1\r\n";
    hdr << "Host: example.com\r\n";
    hdr << "Transfer-Encoding: chunked\r\n";
    hdr << "Content-Encoding: " << GetEncodingStr(enc) << "\r\n";
    hdr << "Connection: close\r\n\r\n";
    hdr << std::hex << firstChunkSize << "\r\n";

    std::string req = hdr.str();
    req.append(comp.data(), firstChunkSize);
    req += "\r\n";
    hdr.str({});
    hdr.clear();
    hdr << std::hex << secondChunkSize << "\r\n";
    req += hdr.str();
    req.append(comp.data() + firstChunkSize, secondChunkSize);
    req += "\r\n0\r\n\r\n";

    test::ClientConnection sock(ts.port());
    NativeHandle fd = sock.fd();
    test::sendAll(fd, req);
    std::string resp = test::recvUntilClosed(fd);

    EXPECT_TRUE(resp.starts_with("HTTP/1.1 413"))
        << "Expected cumulative compressed-size limit to trigger for encoding: " << GetEncodingStr(enc)
        << ", compressed size: " << comp.size() << ", response: " << resp;
  }

  ts.postConfigUpdate([defaultMaxBodyBytes](HttpServerConfig& cfg) { cfg.maxBodyBytes = defaultMaxBodyBytes; });
}

TEST(HttpRequestDecompression, ExpansionRatioGuard) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.maxExpansionRatio = 2.0;
    cfg.decompression.maxDecompressedBytes = 100000;
  });
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });
  // Highly compressible large input -> gzip then send; expect rejection if ratio >2
  std::string large(100000, 'A');

  for (const Encoding enc : test::SupportedEncodings()) {
    auto comp = test::Compress(enc, large);
    // Ensure it actually compresses well
    ASSERT_LT(comp.size() * 2, large.size());
    ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression.maxExpansionRatio = 2.0; });

    auto resp = RawPost(ts.port(), "/rg", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodePayloadTooLarge);

    // Now send with a bigger maxExpansionRatio and expect success
    const double actualExpansionRatio = static_cast<double>(large.size()) / static_cast<double>(comp.size());
    ts.postConfigUpdate([actualExpansionRatio](HttpServerConfig& cfg) {
      cfg.decompression.maxExpansionRatio = actualExpansionRatio + 1;
    });
    resp = RawPost(ts.port(), "/rg_ok", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
    EXPECT_EQ(resp.body, large);
  }
}

TEST(HttpRequestDecompression, StreamingThresholdLargeBody) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.decoderChunkSize = 16;
  });
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });

  std::string plain(4096, 'S');

  for (const Encoding enc : test::SupportedEncodings()) {
    auto comp = test::Compress(enc, plain);
    ts.postConfigUpdate(
        [&comp](HttpServerConfig& cfg) { cfg.decompression.streamingDecompressionThresholdBytes = comp.size(); });
    auto resp = RawPost(ts.port(), "/stream", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);

    // disable streaming
    ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression.streamingDecompressionThresholdBytes = 0; });

    resp = RawPost(ts.port(), "/stream", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);

    // disable streaming based on size smaller than threshold
    ts.postConfigUpdate(
        [&comp](HttpServerConfig& cfg) { cfg.decompression.streamingDecompressionThresholdBytes = comp.size() + 1; });

    resp = RawPost(ts.port(), "/stream", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
  }
}

namespace {
void ExpectTrailers(vector<Encoding> encodings, bool insertBadTrailer = false, bool corruptData = false) {
  std::string plain(10000, 'L');
  std::ranges::iota(plain, 'A');

  ts.router().setDefault([&plain](const HttpRequestView& req) {
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
  for (Encoding encoding : encodings) {
    comp = test::Compress(encoding, comp);
    if (corruptData && encoding == encodings.back() && encoding != Encoding::none) {
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
  NativeHandle fd = sock.fd();

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
  for (const Encoding enc : test::SupportedEncodings()) {
    ExpectTrailers({enc}, true);
  }
}

TEST(HttpRequestDecompression, SingleCompressLargeBodyWithTrailers) {
  static constexpr std::size_t kMaxCompressedBytes[]{0, 128, 1024};
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  for (std::size_t maxCompressedBytes : kMaxCompressedBytes) {
    ts.postConfigUpdate(
        [maxCompressedBytes](HttpServerConfig& cfg) { cfg.decompression.maxCompressedBytes = maxCompressedBytes; });
    for (const Encoding enc : test::SupportedEncodings()) {
      ExpectTrailers({enc});
    }
  }
}

TEST(HttpRequestDecompression, DualCompressLargeBodyWithTrailers) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  for (bool corruptData : {false, true}) {
    for (const Encoding first : test::SupportedEncodings()) {
      for (const Encoding second : test::SupportedEncodings()) {
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
    for (const Encoding first : test::SupportedEncodings()) {
      for (const Encoding second : test::SupportedEncodings()) {
        for (const Encoding third : test::SupportedEncodings()) {
          ExpectTrailers({first, second, third});
        }
      }
    }
  }
}

TEST(HttpRequestDecompression, MixedCaseTokens) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  std::string plain = "CaseCheck";
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });
  std::uniform_int_distribution<int> toUpper(0, 1);

  for (const Encoding enc : test::SupportedEncodings()) {
    std::string mixedCaseEnc(GetEncodingStr(enc));
    for (char& ch : mixedCaseEnc) {
      if (toUpper(rng) == 1) {
        ch = toupper(ch);
      }
    }
    auto comp = test::Compress(enc, plain);
    auto resp = RawPost(ts.port(), "/case", {{"Content-Encoding", mixedCaseEnc}}, comp);
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
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });

  for (std::size_t threshold : kStreamingThresholds) {
    ts.postConfigUpdate(
        [threshold](HttpServerConfig& cfg) { cfg.decompression.streamingDecompressionThresholdBytes = threshold; });
    for (const Encoding enc : test::SupportedEncodings()) {
      auto comp = test::Compress(enc, plain);
      if (enc == Encoding::none) {
        // identity compression cannot be corrupted
        continue;
      }
      test::CorruptData(enc, comp);

      auto resp = RawPost(ts.port(), "/corrupt", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
      EXPECT_EQ(resp.status, http::StatusCodeBadRequest)
          << "Expected 400 for corrupted encoding: " << GetEncodingStr(enc);
    }
  }
}

}  // namespace aeronet