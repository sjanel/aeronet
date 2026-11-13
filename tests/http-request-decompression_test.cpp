#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <ios>
#include <iterator>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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
#include "aeronet/socket-ops.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/supported-accept-encoding.hpp"
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
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.maxExpansionRatio = 1000;
  });
  const std::string plain(10000, 'A');

  for (const Encoding enc : test::SupportedEncodings()) {
    const auto comp = test::Compress(enc, plain);
    const std::size_t compressedSize = comp.size();

    ts.resetRouterAndGet().setDefault([plain, compressedSize, enc](const HttpRequestView& req) {
      EXPECT_EQ(req.body(), plain);
      EXPECT_FALSE(req.headerValue(http::ContentEncoding));
      EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodingHeaderName), GetEncodingStr(enc));
      EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodedLengthHeaderName), std::to_string(compressedSize));

      const auto contentLenghSv = req.headerValueOrEmpty(http::ContentLength);
      uint64_t computedSz{};
      EXPECT_EQ(std::from_chars(contentLenghSv.data(), contentLenghSv.data() + contentLenghSv.size(), computedSz).ec,
                std::errc{});

      EXPECT_EQ(req.body().size(), computedSz);

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

// At most 5 codings are decoded, identity included (curl bounds the codings of a response the same way,
// CVE-2023-23916): a longer list is answered 415 without being decoded.
TEST(HttpRequestDecompression, AtMostFiveStackedCodings) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.resetRouterAndGet().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });
  for (const Encoding enc : test::SupportedEncodings()) {
    RawChars body = test::Compress(enc, "stacked");
    std::string contentEncoding(GetEncodingStr(enc));
    for (int coding = 1; coding < 5; ++coding) {
      body = test::Compress(enc, std::string_view(body));
      contentEncoding.append(", ").append(GetEncodingStr(enc));
    }
    auto resp = RawPost(ts.port(), "/stacked", {{"Content-Encoding", contentEncoding}}, body);
    EXPECT_EQ(resp.status, http::StatusCodeOK) << contentEncoding;
    EXPECT_EQ(resp.body, "stacked") << contentEncoding;

    contentEncoding.insert(0, "identity, ");
    resp = RawPost(ts.port(), "/stacked", {{"Content-Encoding", contentEncoding}}, body);
    EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType) << contentEncoding;
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

namespace {
// Posts 'body' with a chunked framing, in chunks of 3 bytes.
std::string ChunkedPost(std::string_view contentEncoding, std::string_view body) {
  std::string req = std::format(
      "POST /e HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nContent-Encoding: {}\r\nConnection: close\r\n\r\n",
      contentEncoding);
  for (std::size_t pos = 0; pos < body.size(); pos += 3) {
    const std::string_view chunk = body.substr(pos, 3);
    req.append(std::format("{:x}\r\n", chunk.size())).append(chunk).append("\r\n");
  }
  req.append("0\r\n\r\n");
  return test::sendAndCollect(ts.port(), req);
}
}  // namespace

#if defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZSTD)
// RFC 9110 §12.5.3: "servers that fail a request due to an unsupported content coding ought to respond with a 415
// status and include an Accept-Encoding header field in that response", so that the client can tell the refused
// coding from a refused media type, and retry with a supported one. The other errors of the decoding do not list them.
TEST(HttpRequestDecompression, UnsupportedCodingAnswerListsTheSupportedOnes) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.resetRouterAndGet().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });
  const std::string acceptEncodingLine =
      std::format("\r\n{}: {}\r\n", std::string_view(http::AcceptEncoding), internal::kSupportedAcceptEncoding);

  const auto expectListed = [&acceptEncodingLine](std::string_view contentEncoding, std::string_view body) {
    SCOPED_TRACE(contentEncoding);
    const auto resp = RawPost(ts.port(), "/u", {{"Content-Encoding", contentEncoding}}, body);
    EXPECT_EQ(resp.status, http::StatusCodeUnsupportedMediaType);
    EXPECT_TRUE(resp.headersRaw.contains(acceptEncodingLine)) << resp.headersRaw;
    const std::string chunked = ChunkedPost(contentEncoding, body);
    EXPECT_TRUE(chunked.starts_with("HTTP/1.1 415 ")) << chunked;
    EXPECT_TRUE(chunked.contains(acceptEncodingLine)) << chunked;
  };

  expectListed("snappy", "abc");
  for (const Encoding enc : test::SupportedEncodings()) {
    const RawChars compressed = test::Compress(enc, "abc");
    // Refused after the decoding of the last coding applied.
    expectListed(std::format("snappy, {}", GetEncodingStr(enc)), compressed);
    // More codings than decoded.
    expectListed(std::format("identity, identity, identity, identity, identity, {}", GetEncodingStr(enc)), compressed);

    // A body failing to decode is not a refused coding.
    const std::string_view corrupted = std::string_view(compressed).substr(0, compressed.size() / 2);
    const auto resp = RawPost(ts.port(), "/u", {{"Content-Encoding", GetEncodingStr(enc)}}, corrupted);
    EXPECT_EQ(resp.status, http::StatusCodeBadRequest) << GetEncodingStr(enc);
    EXPECT_FALSE(resp.headersRaw.contains(std::string_view(http::AcceptEncoding))) << resp.headersRaw;
  }
}
#endif

// RFC 9110 §5.6.1.2: a recipient MUST ignore a reasonable number of empty list elements (they count towards the limit
// of 5 codings). A Content-Encoding without any coding is identity: the body is passed as received.
TEST(HttpRequestDecompression, EmptyElementsAreIgnored) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.resetRouterAndGet().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });
  static constexpr std::string_view kBody = "xyz";
  for (std::string_view contentEncoding : {"identity,,identity", "identity,,", ",", ""}) {
    SCOPED_TRACE(contentEncoding);
    auto resp = RawPost(ts.port(), "/e", {{"Content-Encoding", contentEncoding}}, kBody);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
    EXPECT_EQ(resp.body, kBody);
    EXPECT_TRUE(ChunkedPost(contentEncoding, kBody).ends_with("\r\n\r\nxyz"));
  }
  for (const Encoding enc : test::SupportedEncodings()) {
    const RawChars compressed = test::Compress(enc, "decoded body");
    const std::string contentEncoding = std::format(", {},,", GetEncodingStr(enc));
    SCOPED_TRACE(contentEncoding);
    auto resp = RawPost(ts.port(), "/e", {{"Content-Encoding", contentEncoding}}, compressed);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
    EXPECT_EQ(resp.body, "decoded body");
    EXPECT_TRUE(ChunkedPost(contentEncoding, compressed).ends_with("\r\n\r\ndecoded body"));
  }
}

#ifdef AERONET_ENABLE_ZLIB
// RFC 9110 §8.4.1.3: "A recipient SHOULD consider "x-gzip" to be equivalent to "gzip"".
TEST(HttpRequestDecompression, XGzipIsDecodedAsGzip) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.resetRouterAndGet().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });
  const RawChars compressed = test::Compress(Encoding::gzip, "decoded body");
  auto resp = RawPost(ts.port(), "/e", {{"Content-Encoding", "x-gzip"}}, compressed);
  EXPECT_EQ(resp.status, http::StatusCodeOK);
  EXPECT_EQ(resp.body, "decoded body");
  EXPECT_TRUE(ChunkedPost("X-Gzip", compressed).ends_with("\r\n\r\ndecoded body"));
}
#endif

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

// The compressed size of a body with Content-Length is known from its head: above maxCompressedBytes, it is refused
// before it is received (as for a chunked body, refused from the size line of the chunk exceeding it), instead of
// buffered up to maxBodyBytes and then refused. Also when its client waits for 100 Continue, which is not sent.
TEST(HttpRequestDecompression, MaxCompressedBytesOfAContentLengthBodyIsCheckedFromTheHead) {
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });
  const std::string plain = test::MakePatternedPayload(1000);

  for (const Encoding enc : test::SupportedEncodings()) {
    const RawChars comp = test::Compress(enc, plain);
    ts.postConfigUpdate([&comp](HttpServerConfig& cfg) {
      cfg.decompression = {};
      cfg.decompression.maxCompressedBytes = comp.size();
    });

    // Only the head is sent: the answer cannot wait for the body.
    for (const std::string_view expect : {"", "Expect: 100-continue\r\n"}) {
      for (const std::string_view coding : {GetEncodingStr(enc), std::string_view("identity, gzip")}) {
        test::ClientConnection cnx(ts.port());
        test::sendAll(cnx.fd(), std::format("POST /head HTTP/1.1\r\nHost: h\r\nContent-Encoding: {}\r\n{}"
                                            "Content-Length: {}\r\n\r\n",
                                            coding, expect, comp.size() + 1U));
        const std::string resp = test::recvWithTimeout(cnx.fd(), std::chrono::milliseconds{2000});
        EXPECT_TRUE(resp.starts_with("HTTP/1.1 413")) << coding << " '" << expect << "': " << resp;
      }
    }

    // At the limit: received and decompressed.
    auto resp = RawPost(ts.port(), "/limit", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK) << GetEncodingStr(enc);
    EXPECT_EQ(resp.body, plain) << GetEncodingStr(enc);

    // Above it, a body without coding to decode is not bounded by it.
    const std::string larger(comp.size() + 1U, 'L');
    for (const std::string_view coding : {std::string_view("identity"), std::string_view(", identity")}) {
      resp = RawPost(ts.port(), "/identity", {{"Content-Encoding", coding}}, larger);
      EXPECT_EQ(resp.status, http::StatusCodeOK) << coding;
      EXPECT_EQ(resp.body, larger) << coding;
    }
    resp = RawPost(ts.port(), "/none", {}, larger);
    EXPECT_EQ(resp.status, http::StatusCodeOK);
    EXPECT_EQ(resp.body, larger);
  }
}

TEST(HttpRequestDecompression, ChunkedCompressedBodyExceedsMaxBodyBytesCumulatively) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.decompression = {};
    cfg.decompression.maxCompressedBytes = std::numeric_limits<std::size_t>::max();
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

// The chunks of a compressed body received by several reads are decoded as they come, then decompressed together: the
// result is the one of a body received at once.
TEST(HttpRequestDecompression, ChunkedCompressedBodyReceivedInSeveralReads) {
  using namespace std::chrono_literals;

  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.router().setDefault([](const HttpRequestView& req) {
    return HttpResponse(std::format("{}|{}", req.trailerValueOrEmpty("x-checksum"), req.body()));
  });

  std::string plain;
  std::uniform_int_distribution<int> letter('a', 'p');
  while (plain.size() < 30000) {
    plain.push_back(static_cast<char>(letter(rng)));
  }

  for (const Encoding enc : test::SupportedEncodings()) {
    const auto comp = test::Compress(enc, plain);
    SCOPED_TRACE(GetEncodingStr(enc));
    ASSERT_GT(comp.size(), 3000U);

    for (const bool withTrailer : {false, true}) {
      SCOPED_TRACE(withTrailer);
      std::string req = "POST /resume HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nContent-Encoding: ";
      req.append(GetEncodingStr(enc));
      req.append("\r\nConnection: close\r\n\r\n");
      static constexpr std::size_t kChunkSize = 1000;
      for (std::size_t pos = 0; pos < comp.size(); pos += kChunkSize) {
        const std::size_t chunkSize = std::min<std::size_t>(kChunkSize, comp.size() - pos);
        req.append(std::format("{:x}\r\n", chunkSize));
        req.append(comp.data() + pos, chunkSize);
        req.append("\r\n");
      }
      req.append(withTrailer ? "0\r\nX-Checksum: abc123\r\n\r\n" : "0\r\n\r\n");

      // Parts of a different size than the chunks: the reads end in the chunk data, the size lines and the trailers.
      test::ClientConnection sock(ts.port());
      NativeHandle fd = sock.fd();
      ASSERT_TRUE(SetTcpNoDelay(fd));
      static constexpr std::size_t kPartSize = 700;
      for (std::size_t pos = 0; pos < req.size(); pos += kPartSize) {
        test::sendAll(fd, std::string_view(req).substr(pos, kPartSize));
        std::this_thread::sleep_for(1ms);
      }
      const std::string resp = test::recvUntilClosed(fd);
      ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp.substr(0, 100);
      EXPECT_TRUE(resp.ends_with(std::format("{}|{}", withTrailer ? "abc123" : "", plain)));
    }
  }
}

// A chunked body without any chunk has nothing to decompress, whatever its Content-Encoding (trailers are kept).
TEST(HttpRequestDecompression, ChunkedEmptyBodyWithContentEncoding) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.router().setDefault([](const HttpRequestView& req) {
    return HttpResponse(std::format("[{}|{}]", req.body(), req.trailerValueOrEmpty("x-checksum")));
  });

  for (const Encoding enc : test::SupportedEncodings()) {
    SCOPED_TRACE(GetEncodingStr(enc));
    for (const bool withTrailer : {false, true}) {
      SCOPED_TRACE(withTrailer);
      std::string req = "POST /empty HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nContent-Encoding: ";
      req.append(GetEncodingStr(enc));
      req.append("\r\nConnection: close\r\n\r\n");
      req.append(withTrailer ? "0\r\nX-Checksum: abc123\r\n\r\n" : "0\r\n\r\n");

      const std::string resp = test::sendAndCollect(ts.port(), req);
      ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
      EXPECT_TRUE(resp.ends_with(withTrailer ? "[|abc123]" : "[|]")) << resp;
    }
  }
}

// The trailers of a compressed body, moved behind the decompressed body, are parsed once: the merge of a duplicate
// leaves nothing behind it, and a duplicate that cannot be merged is rejected. Also for a body decompressing to
// nothing.
TEST(HttpRequestDecompression, DuplicateTrailersOfCompressedBody) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  ts.router().setDefault([](const HttpRequestView& req) {
    return HttpResponse(
        std::format("[{}|{}|{}]", req.body(), req.trailers().size(), req.trailerValueOrEmpty("accept")));
  });

  for (const Encoding enc : test::SupportedEncodings()) {
    SCOPED_TRACE(GetEncodingStr(enc));
    for (const std::string_view plain : {"", "HelloCompressedWorld"}) {
      SCOPED_TRACE(plain);
      const auto comp = test::Compress(enc, plain);
      ASSERT_FALSE(comp.empty());  // an empty chunk would be the last one
      const auto send = [&](std::string_view trailerLines) {
        std::string req = "POST /dup HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nContent-Encoding: ";
        req.append(GetEncodingStr(enc));
        req.append(std::format("\r\nConnection: close\r\n\r\n{:x}\r\n", comp.size()));
        req.append(comp);
        req.append(std::format("\r\n0\r\n{}\r\n", trailerLines));
        return test::sendAndCollect(ts.port(), req);
      };

      std::string resp = send("accept: text/html\r\naccept: application/json\r\n");
      ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
      EXPECT_TRUE(resp.ends_with(std::format("[{}|1|text/html,application/json]", plain))) << resp;

      resp = send("from: a@example.com\r\nfrom: b@example.com\r\n");
      EXPECT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
    }
  }
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
    cfg.decompression.maxExpansionRatio = 1000;
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
    ts.postConfigUpdate([](HttpServerConfig& cfg) {
      cfg.decompression.streamingDecompressionThresholdBytes = std::numeric_limits<std::size_t>::max();
    });

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
    auto it = req.trailers().find("x-checksum");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      EXPECT_EQ(it->second, "abc123");
    }
    auto it2 = req.trailers().find("x-note");
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
    if (comp.size() > maxBodyBytes || comp.size() > maxCompressedBytes) {
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
  std::uniform_int_distribution<int> toUpper(0, 1);

  for (const Encoding enc : test::SupportedEncodings()) {
    std::string mixedCaseEnc(GetEncodingStr(enc));
    for (char& ch : mixedCaseEnc) {
      if (toUpper(rng) == 1) {
        ch = toupper(ch);
      }
    }
    mixedCaseEnc.front() = toupper(mixedCaseEnc.front());
    // Codings are lower-cased when parsed: the handler sees them normalized.
    ts.resetRouterAndGet().setDefault([enc](const HttpRequestView& req) {
      EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodingHeaderName), GetEncodingStr(enc));
      return HttpResponse(req.body());
    });
    auto comp = test::Compress(enc, plain);
    auto resp = RawPost(ts.port(), "/case", {{"Content-Encoding", mixedCaseEnc}}, comp);
    EXPECT_EQ(resp.status, http::StatusCodeOK) << mixedCaseEnc;
    EXPECT_EQ(resp.body, plain) << mixedCaseEnc;
  }
}

TEST(HttpRequestDecompression, MixedCaseTokensChunked) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  const std::string plain(4096, 'C');

  for (const Encoding enc : test::SupportedEncodings()) {
    std::string upperCaseEnc(GetEncodingStr(enc));
    std::ranges::transform(upperCaseEnc, upperCaseEnc.begin(), [](char ch) { return toupper(ch); });
    const std::string contentEncoding = "IDENTITY, " + upperCaseEnc;
    ts.resetRouterAndGet().setDefault([&plain, enc](const HttpRequestView& req) {
      EXPECT_EQ(req.body(), plain);
      EXPECT_EQ(req.headerValueOrEmpty(http::OriginalEncodingHeaderName),
                std::string("identity, ").append(GetEncodingStr(enc)));
      return HttpResponse("chunked case OK");
    });

    const auto comp = test::Compress(enc, plain);
    char sizeHex[16];
    const auto [sizeHexEnd, errc] = std::to_chars(sizeHex, std::end(sizeHex), comp.size(), 16);
    ASSERT_EQ(errc, std::errc{});

    std::string req = "POST /case HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n";
    req += MakeHttp1HeaderLine("Content-Encoding", contentEncoding);
    req += http::CRLF;
    req.append(sizeHex, sizeHexEnd);
    req += http::CRLF;
    req.append(std::string_view(comp));
    req += "\r\n0\r\n\r\n";

    test::ClientConnection sock(ts.port());
    test::sendAll(sock.fd(), req);
    const std::string resp = test::recvUntilClosed(sock.fd());
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << contentEncoding << ": " << resp;
    EXPECT_TRUE(resp.ends_with("\r\n\r\nchunked case OK")) << contentEncoding << ": " << resp;
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

// A body whose compressed stream was cut short is rejected, never handed truncated to the handler.
TEST(HttpRequestDecompression, TruncatedCompressedData) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression = {}; });
  const std::string plain = test::MakePatternedPayload(20000);
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });

  for (std::size_t threshold : kStreamingThresholds) {
    ts.postConfigUpdate(
        [threshold](HttpServerConfig& cfg) { cfg.decompression.streamingDecompressionThresholdBytes = threshold; });
    for (const Encoding enc : test::SupportedEncodings()) {
      const RawChars comp = test::Compress(enc, plain);
      const std::string_view truncated = std::string_view(comp).substr(0, comp.size() - 1);
      auto resp = RawPost(ts.port(), "/truncated", {{"Content-Encoding", GetEncodingStr(enc)}}, truncated);
      EXPECT_EQ(resp.status, http::StatusCodeBadRequest)
          << "Expected 400 for truncated encoding: " << GetEncodingStr(enc) << " threshold " << threshold;
    }
  }
}

// maxDecompressedBytes bounds the whole decoded body (413): with a Content-Length, decoded in one shot or in streaming,
// or chunked, decoded chunk by chunk.
TEST(HttpRequestDecompression, MaxDecompressedBytesBoundsTheWholeBody) {
  const RawChars plain = test::MakeRandomPayload(20000);  // incompressible: decoded in many pieces
  ts.router().setDefault([](const HttpRequestView& req) { return HttpResponse(req.body()); });

  for (const std::size_t limit : {plain.size(), plain.size() - 1}) {
    const http::StatusCode expected = limit == plain.size() ? http::StatusCodeOK : http::StatusCodePayloadTooLarge;
    for (const std::size_t threshold : {std::size_t{1}, std::numeric_limits<std::size_t>::max()}) {
      ts.postConfigUpdate([limit, threshold](HttpServerConfig& cfg) {
        cfg.maxBodyBytes = 1UL << 20U;  // the compressed body is never the limit here
        cfg.decompression = {};
        cfg.decompression.decoderChunkSize = 1024;
        cfg.decompression.maxDecompressedBytes = limit;
        cfg.decompression.streamingDecompressionThresholdBytes = threshold;
      });
      for (const Encoding enc : test::SupportedEncodings()) {
        const RawChars comp = test::Compress(enc, std::string_view(plain));
        const auto resp = RawPost(ts.port(), "/limit", {{"Content-Encoding", GetEncodingStr(enc)}}, comp);
        EXPECT_EQ(resp.status, expected) << GetEncodingStr(enc) << " limit " << limit << " threshold " << threshold;
        if (expected == http::StatusCodeOK) {
          EXPECT_TRUE(resp.body == std::string_view(plain)) << GetEncodingStr(enc);
        }
      }
    }

    for (const Encoding enc : test::SupportedEncodings()) {
      const RawChars comp = test::Compress(enc, std::string_view(plain));
      std::string req("POST /limit HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n");
      req += MakeHttp1HeaderLine(http::ContentEncoding, GetEncodingStr(enc));
      req += http::CRLF;
      for (std::size_t off = 0; off < comp.size(); off += 1000) {
        const std::string_view chunk = std::string_view(comp).substr(off, 1000);
        char hex[16];
        const auto [ptr, errc] = std::to_chars(hex, hex + sizeof(hex), chunk.size(), 16);
        ASSERT_EQ(errc, std::errc());
        req.append(hex, ptr);
        req += http::CRLF;
        req += chunk;
        req += http::CRLF;
      }
      req += "0\r\n\r\n";
      const std::string resp = test::sendAndCollect(ts.port(), req);
      EXPECT_TRUE(resp.starts_with(expected == http::StatusCodeOK ? "HTTP/1.1 200" : "HTTP/1.1 413"))
          << "chunked " << GetEncodingStr(enc) << " limit " << limit << ": " << resp.substr(0, 40);
    }
  }
}

}  // namespace aeronet