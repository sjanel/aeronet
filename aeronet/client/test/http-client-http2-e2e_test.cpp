// End-to-end coverage of the native HTTP/2 client engine against a live aeronet server:
// prior-knowledge h2c over plain http (HttpVersionMode::Http2), the Auto cleartext fallback to
// HTTP/1.1, and -- with OpenSSL -- ALPN negotiation over https for every HttpVersionMode.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/aeronet.hpp"
#include "aeronet/client-connection.hpp"
#include "aeronet/client-protocol.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <csignal>

#include "aeronet/test-tls-helper.hpp"
#endif

namespace aeronet {

class HttpRequestTest {
 public:
  static void Finalize(HttpRequest& req) { req.HttpMessage::finalizeHeadersAndBody(); }
};

namespace {

constexpr std::size_t kLargeBodySize = 1UL << 20;  // 1 MiB: many DATA frames, several flow-control windows

std::string MakeLargeBody() {
  std::string body(kLargeBodySize, '\0');
  for (std::size_t charPos = 0; charPos < body.size(); ++charPos) {
    body[charPos] = static_cast<char>('A' + (charPos % 53));
  }
  return body;
}

std::atomic<bool> lastSeenHttp2{false};

// NOTE: HttpRequestView::clientAddress() is not usable here: requests dispatched through the HTTP/2
// handler carry no owner state (pre-existing server-side gap), so only the version is recorded.
void Observe(const HttpRequestView& req) {
  lastSeenHttp2.store(req.version() == http::HTTP_2_0, std::memory_order_relaxed);
}

HttpClient MakeHttp2Client() { return HttpClient(HttpClientConfig{}.withHttpVersion(HttpVersionMode::Http2)); }

test::TestServer CreateTestServer() {
  test::TestServer testServer(HttpServerConfig{}
                                  .withPort(0)
                                  .withKeepAliveTimeout(std::chrono::seconds{5})
                                  .withPollInterval(std::chrono::milliseconds{20}));

  auto routerProxy = testServer.router();
  routerProxy.setPath(http::Method::GET, "/hello", [](const HttpRequestView& req) {
    Observe(req);
    return req.makeResponse(http::StatusCodeOK, "world", "text/plain");
  });
  routerProxy.setPath(http::Method::HEAD, "/hello", [](const HttpRequestView& req) {
    Observe(req);
    return req.makeResponse(http::StatusCodeOK, "world", "text/plain");
  });
  routerProxy.setPath(http::Method::POST, "/echo", [](const HttpRequestView& req) {
    Observe(req);
    return req.makeResponse(http::StatusCodeOK, req.body(), "application/test");
  });
  routerProxy.setPath(http::Method::GET, "/big", [](const HttpRequestView& req) {
    Observe(req);
    return req.makeResponse(http::StatusCodeOK, MakeLargeBody(), "application/octet-stream");
  });
  routerProxy.setPath(http::Method::GET, "/redirect", [](const HttpRequestView& req) {
    Observe(req);
    return req.makeResponse(http::StatusCodeFound, "", "text/plain").location("/hello");
  });
  routerProxy.setPath(http::Method::POST, "/see-other", [](const HttpRequestView& req) {
    Observe(req);
    return req.makeResponse(http::StatusCodeSeeOther, "", "text/plain").location("/hello");
  });
  routerProxy.setPath(http::Method::GET, "/headers", [](const HttpRequestView& req) {
    Observe(req);
    auto resp = req.makeResponse(http::StatusCodeOK, "hdr", "text/plain");
    resp.header("x-echoed", req.headerValueOrEmpty("x-custom-token"));
    resp.header("x-server", "aeronet");
    return resp;
  });
  // 303 See Other -> rewrite to GET /headers with the request body dropped. A user header set on the
  // original POST must survive the drop-body rewrite and reach /headers (where it is reflected back).
  routerProxy.setPath(http::Method::POST, "/see-other-headers", [](const HttpRequestView& req) {
    Observe(req);
    return req.makeResponse(http::StatusCodeSeeOther, "", "text/plain").location("/headers");
  });
  // Reflect the received request trailers (RFC 9113 §8.1): the response body echoes the decoded request
  // body, and the observed trailer values + count are surfaced as response headers.
  routerProxy.setPath(http::Method::POST | http::Method::PUT, "/trailer-echo", [](const HttpRequestView& req) {
    Observe(req);
    auto resp = req.makeResponse(http::StatusCodeOK, req.body(), "text/plain");
    resp.header("echo-checksum", req.trailerValueOrEmpty("x-checksum"));
    resp.header("echo-signature", req.trailerValueOrEmpty("x-signature"));
    resp.header("echo-trailer-count", req.trailers().size());
    return resp;
  });

  return testServer;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer ts = CreateTestServer();
const auto port = ts.port();

[[nodiscard]] std::string Url(std::string_view path) {
  return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
}

class ScriptedHttp2Transport final : public ITransport {
 public:
  enum class Action : uint8_t {
    RstStream,
    GoAway,
    CloseOnRead,
    WriteError,
    WriteReadReady,
    WriteWriteReady,
    ReadWriteReady,
  };

  explicit ScriptedHttp2Transport(Action action) : _action(action) {
    if (action != Action::RstStream && action != Action::GoAway) {
      return;
    }
    RawBytes settings;
    http2::WriteSettingsFrame(settings, std::span<const http2::SettingsEntry>{});
    _reads.emplace_back(reinterpret_cast<const char*>(settings.data()), settings.size());

    RawBytes fault;
    if (action == Action::RstStream) {
      http2::WriteRstStreamFrame(fault, 1, http2::ErrorCode::Cancel);
    } else {
      http2::WriteGoAwayFrame(fault, 0, http2::ErrorCode::NoError, "maintenance");
    }
    _reads.emplace_back(reinterpret_cast<const char*>(fault.data()), fault.size());
  }

  TransportResult read(char* buf, std::size_t len) override {
    if (_action == Action::ReadWriteReady) {
      return {0, TransportHint::WriteReady};
    }
    if (_nextRead == _reads.size()) {
      return {0, TransportHint::None};
    }
    const std::string& data = _reads[_nextRead++];
    if (data.size() > len) {
      ADD_FAILURE() << "scripted HTTP/2 frame exceeds the client read buffer";
      return {0, TransportHint::Error};
    }
    std::ranges::copy(data, buf);
    return {data.size(), TransportHint::None};
  }

  TransportResult write(std::string_view data) override {
    switch (_action) {
      case Action::WriteError:
        return {0, TransportHint::Error};
      case Action::WriteReadReady:
        return {0, TransportHint::ReadReady};
      case Action::WriteWriteReady:
        return {0, TransportHint::WriteReady};
      default:
        _bytesWritten += data.size();
        return {data.size(), TransportHint::None};
    }
  }

  [[nodiscard]] std::size_t bytesWritten() const noexcept { return _bytesWritten; }

 private:
  Action _action;
  std::vector<std::string> _reads;
  std::size_t _nextRead{0};
  std::size_t _bytesWritten{0};
};

class LoopbackHttp2Transport final : public ITransport {
 public:
  enum class ResponseMode : uint8_t {
    MissingStatus,
    InvalidStatus,
    InterimThenOk,
    EmptyData,
    NoContentType,
    EarlyResponse,
  };

  LoopbackHttp2Transport(const Http2Config& config, ResponseMode responseMode)
      : _server(config, /*isServer=*/true), _responseMode(responseMode) {
    _server.setOnHeadersDecoded([this](uint32_t streamId, const HeadersViewMap&, bool) {
      if (!_responded) {
        _responded = true;
        respond(streamId);
      }
    });
    _server.setOnStreamReset([this](uint32_t, http2::ErrorCode) { _sawReset = true; });
  }

  TransportResult read(char* buf, std::size_t len) override {
    if (!_server.hasPendingOutput()) {
      return {0, TransportHint::Error};
    }
    const std::span<const std::byte> output = _server.getPendingOutput();
    const http2::FrameHeader header = http2::ParseFrameHeader(output);
    const std::size_t frameSize = http2::FrameHeader::kSize + header.length;
    if (frameSize > len) {
      ADD_FAILURE() << "loopback HTTP/2 frame exceeds the client read buffer";
      return {0, TransportHint::Error};
    }
    std::memcpy(buf, output.data(), frameSize);
    _server.onOutputWritten(frameSize);
    return {frameSize, TransportHint::None};
  }

  TransportResult write(std::string_view data) override {
    _clientInput.append(data);
    while (!_clientInput.empty()) {
      const auto input = std::as_bytes(std::span<const char>(_clientInput.data(), _clientInput.size()));
      const auto processed = _server.processInput(input);
      EXPECT_NE(processed.action, http2::Http2Connection::ProcessResult::Action::Error) << processed.errorMessage;
      if (processed.bytesConsumed == 0) {
        break;
      }
      _clientInput.erase_front(processed.bytesConsumed);
    }
    _bytesWritten += data.size();
    return {data.size(), TransportHint::None};
  }

  [[nodiscard]] bool sawReset() const noexcept { return _sawReset; }
  [[nodiscard]] std::size_t bytesWritten() const noexcept { return _bytesWritten; }

 private:
  void respond(uint32_t streamId) {
    switch (_responseMode) {
      case ResponseMode::MissingStatus:
        EXPECT_EQ(_server.sendHeaders(streamId, http::StatusCode{}, HeadersView{}, true), http2::ErrorCode::NoError);
        break;
      case ResponseMode::InvalidStatus: {
        RawChars headers;
        headers.append(":status: 099\r\n");
        EXPECT_EQ(_server.sendHeaders(streamId, http::StatusCode{}, HeadersView(headers), true),
                  http2::ErrorCode::NoError);
        break;
      }
      case ResponseMode::InterimThenOk:
        EXPECT_EQ(_server.sendHeaders(streamId, http::StatusCodeContinue, HeadersView{}, false),
                  http2::ErrorCode::NoError);
        EXPECT_EQ(_server.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, true), http2::ErrorCode::NoError);
        break;
      case ResponseMode::EmptyData: {
        RawChars headers;
        headers.append("content-type: text/plain\r\n");
        EXPECT_EQ(_server.sendHeaders(streamId, http::StatusCodeOK, HeadersView(headers), false),
                  http2::ErrorCode::NoError);
        EXPECT_EQ(_server.sendData(streamId, {}, true), http2::ErrorCode::NoError);
        break;
      }
      case ResponseMode::NoContentType: {
        static constexpr std::byte kBody[]{std::byte{'r'}, std::byte{'a'}, std::byte{'w'}};
        EXPECT_EQ(_server.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, false), http2::ErrorCode::NoError);
        EXPECT_EQ(_server.sendData(streamId, kBody, true), http2::ErrorCode::NoError);
        break;
      }
      case ResponseMode::EarlyResponse:
        EXPECT_EQ(_server.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, true), http2::ErrorCode::NoError);
        break;
    }
  }

  http2::Http2Connection _server;
  ResponseMode _responseMode;
  RawChars _clientInput;
  std::size_t _bytesWritten{0};
  bool _responded{false};
  bool _sawReset{false};
};

HttpRequest MakeFinalizedHttp2Request(HttpClient& client) {
  auto req = client.makeRequest(http::Method::GET, "http://example.test/resource");
  HttpRequestTest::Finalize(req);
  return req;
}

}  // namespace

TEST(HttpClientHttp2TransportTest, PeerResetAndGoAwayAbortCurrentExchange) {
  HttpClientConfig config;
  config.withHttpVersion(HttpVersionMode::Http2);
  HttpClient client(config);

  for (const auto action : {ScriptedHttp2Transport::Action::RstStream, ScriptedHttp2Transport::Action::GoAway}) {
    HttpRequest req = MakeFinalizedHttp2Request(client);

    ScriptedHttp2Transport transport(action);
    internal::ClientConnection connection(config.http2);
    bool requestSent = false;

    auto result = connection.exchange(client, transport, kInvalidHandle, req, SteadyClock::now(), requestSent);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), HttpClientErrc::connectionClosed);
    EXPECT_TRUE(requestSent);
    EXPECT_GT(transport.bytesWritten(), 0U);
  }
}

TEST(HttpClientHttp2TransportTest, WriteFailureAndReadinessTimeoutBeforeSending) {
  HttpClientConfig config;
  config.withHttpVersion(HttpVersionMode::Http2);
  HttpClient client(config);

  for (const auto action : {
           ScriptedHttp2Transport::Action::WriteError,
           ScriptedHttp2Transport::Action::WriteReadReady,
           ScriptedHttp2Transport::Action::WriteWriteReady,
       }) {
    HttpRequest req = MakeFinalizedHttp2Request(client);
    ScriptedHttp2Transport transport(action);
    internal::ClientConnection connection(config.http2);
    bool requestSent = false;

    auto result = connection.exchange(client, transport, kInvalidHandle, req, SteadyClock::now(), requestSent);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), action == ScriptedHttp2Transport::Action::WriteError ? HttpClientErrc::writeError
                                                                                   : HttpClientErrc::timeout);
    EXPECT_FALSE(requestSent);
  }
}

TEST(HttpClientHttp2TransportTest, ReadCloseAndWriteReadinessAreReported) {
  HttpClientConfig config;
  config.withHttpVersion(HttpVersionMode::Http2);
  HttpClient client(config);

  for (const auto action :
       {ScriptedHttp2Transport::Action::CloseOnRead, ScriptedHttp2Transport::Action::ReadWriteReady}) {
    HttpRequest req = MakeFinalizedHttp2Request(client);

    ScriptedHttp2Transport transport(action);
    internal::ClientConnection connection(config.http2);
    bool requestSent = false;

    auto result = connection.exchange(client, transport, kInvalidHandle, req, SteadyClock::now(), requestSent);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), action == ScriptedHttp2Transport::Action::CloseOnRead ? HttpClientErrc::connectionClosed
                                                                                    : HttpClientErrc::timeout);
    EXPECT_TRUE(requestSent);
  }
}

TEST(HttpClientHttp2TransportTest, MissingAndInvalidStatusAreMalformedResponses) {
  HttpClientConfig config;
  config.withHttpVersion(HttpVersionMode::Http2);
  HttpClient client(config);

  for (const auto mode :
       {LoopbackHttp2Transport::ResponseMode::MissingStatus, LoopbackHttp2Transport::ResponseMode::InvalidStatus}) {
    HttpRequest req = MakeFinalizedHttp2Request(client);
    LoopbackHttp2Transport transport(config.http2, mode);
    internal::ClientConnection connection(config.http2);
    bool requestSent = false;

    auto result = connection.exchange(client, transport, kInvalidHandle, req, SteadyClock::now(), requestSent);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
    EXPECT_TRUE(requestSent);
  }
}

TEST(HttpClientHttp2TransportTest, InterimHeadersAndEmptyDataCompleteSuccessfully) {
  HttpClientConfig config;
  config.withHttpVersion(HttpVersionMode::Http2);
  HttpClient client(config);

  for (const auto mode :
       {LoopbackHttp2Transport::ResponseMode::InterimThenOk, LoopbackHttp2Transport::ResponseMode::EmptyData}) {
    HttpRequest req = MakeFinalizedHttp2Request(client);

    LoopbackHttp2Transport transport(config.http2, mode);
    internal::ClientConnection connection(config.http2);
    bool requestSent = false;

    auto result = connection.exchange(client, transport, kInvalidHandle, req, SteadyClock::now(), requestSent);

    ASSERT_TRUE(result);
    EXPECT_EQ(result->status(), 200);
    EXPECT_TRUE(result->bodyInMemory().empty());
    EXPECT_TRUE(requestSent);
  }
}

TEST(HttpClientHttp2TransportTest, MissingContentTypeDefaultsToOctetStream) {
  HttpClientConfig config;
  config.withHttpVersion(HttpVersionMode::Http2);
  HttpClient client(config);
  HttpRequest req = MakeFinalizedHttp2Request(client);
  LoopbackHttp2Transport transport(config.http2, LoopbackHttp2Transport::ResponseMode::NoContentType);
  internal::ClientConnection connection(config.http2);
  bool requestSent = false;

  auto result = connection.exchange(client, transport, kInvalidHandle, req, SteadyClock::now(), requestSent);

  ASSERT_TRUE(result);
  EXPECT_EQ(result->bodyInMemory(), "raw");
  EXPECT_EQ(result->headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationOctetStream);
}

TEST(HttpClientHttp2TransportTest, EarlyResponseResetsUnfinishedUpload) {
  HttpClientConfig config;
  config.withHttpVersion(HttpVersionMode::Http2);
  HttpClient client(config);
  auto req = client.makeRequest(http::Method::POST, "http://example.test/upload");
  req.body(std::string(1UL << 20, 'x'), "application/octet-stream");
  HttpRequestTest::Finalize(req);
  LoopbackHttp2Transport transport(config.http2, LoopbackHttp2Transport::ResponseMode::EarlyResponse);
  internal::ClientConnection connection(config.http2);
  bool requestSent = false;

  auto result = connection.exchange(client, transport, kInvalidHandle, req, SteadyClock::now(), requestSent);

  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 200);
  EXPECT_TRUE(transport.sawReset());
  EXPECT_GT(transport.bytesWritten(), 0U);
}

TEST(HttpClientHttp2E2ETest, PriorKnowledgeSimpleGet) {
  HttpClient client = MakeHttp2Client();
  auto resp = client.get(Url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
  EXPECT_TRUE(lastSeenHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2E2ETest, AutoModeStaysHttp11OnCleartext) {
  HttpClient client;  // default HttpVersionMode::Auto
  auto resp = client.get(Url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
  EXPECT_FALSE(lastSeenHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2E2ETest, Http11ModeStillWorks) {
  HttpClient client(HttpClientConfig{}.withHttpVersion(HttpVersionMode::Http1_1));
  auto resp = client.get(Url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_FALSE(lastSeenHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2E2ETest, NotFoundIsAResponseNotAnError) {
  HttpClient client = MakeHttp2Client();
  // No route matches, so observe() never runs: only the status is asserted.
  auto resp = client.get(Url("/does-not-exist")).value();
  EXPECT_EQ(resp.status(), 404);
}

TEST(HttpClientHttp2E2ETest, PostEchoSmallBody) {
  HttpClient client = MakeHttp2Client();
  auto resp = client.post(Url("/echo"), "ping", "text/plain").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "ping");
}

TEST(HttpClientHttp2E2ETest, PostEchoLargeBodyExercisesSendFlowControl) {
  // 1 MiB upload: far beyond the default 65535-byte stream window, so the engine must repeatedly stall
  // on flow control and resume on the server's WINDOW_UPDATEs.
  const std::string payload = MakeLargeBody();
  HttpClient client = MakeHttp2Client();
  auto resp = client.post(Url("/echo"), payload, "application/octet-stream").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), payload);
}

// --- Request file bodies ---------------------------------------------------
// Over HTTP/2 a file body cannot be sent with sendfile(2): the payload must be split into DATA frames
// (flow control + max frame size), so the engine reads the file in bounded chunks and frames them,
// exactly like the server reads file responses. The server echoes back the exact file bytes.

TEST(HttpClientHttp2E2ETest, PostFileBodySmall) {
  static constexpr std::string_view kPayload = "small-http2-file-body";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);

  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, Url("/echo")).file(std::move(file), "application/test");
  auto resp = client.request(std::move(req)).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), kPayload);
  EXPECT_TRUE(lastSeenHttp2.load(std::memory_order_relaxed));
}

// A 1 MiB file body forces many DATA frames across several flow-control windows and multiple file reads
// (the per-flush cap is 64 KiB): the engine must stall on flow control, resume on WINDOW_UPDATEs, and
// reassemble the file intact on the server.
TEST(HttpClientHttp2E2ETest, PostFileBodyLargeExercisesSendFlowControl) {
  const std::string payload = MakeLargeBody();
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, payload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);

  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, Url("/echo")).file(std::move(file), "application/octet-stream");
  auto resp = client.request(std::move(req)).value();
  EXPECT_EQ(resp.status(), 200);
  ASSERT_EQ(resp.bodyInMemory().size(), payload.size());
  EXPECT_EQ(resp.bodyInMemory(), payload);
}

// An empty file body frames as END_STREAM on the HEADERS block (no DATA frame), Content-Length: 0.
TEST(HttpClientHttp2E2ETest, PostEmptyFileBody) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, std::string_view{});
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  ASSERT_EQ(file.size(), 0U);

  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, Url("/echo")).file(std::move(file), "application/test");
  auto resp = client.request(std::move(req)).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

// A file body restricted to a [offset, offset+length) sub-range uploads only that slice.
TEST(HttpClientHttp2E2ETest, PostFileBodyWithOffsetAndLength) {
  const std::string payload = MakeLargeBody();  // 1 MiB, position-dependent
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, payload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);

  constexpr std::size_t kOffset = 1000;
  constexpr std::size_t kLength = 200UL * 1024;  // spans several DATA frames
  HttpClient client = MakeHttp2Client();
  auto req =
      client.makeRequest(http::Method::POST, Url("/echo")).file(std::move(file), kOffset, kLength, "application/test");
  auto resp = client.request(std::move(req)).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), std::string_view(payload).substr(kOffset, kLength));
}

// The HTTP/2 file-body loop must abort when the file cannot be fully read: truncating the file after the
// request is built makes the first readAt() hit EOF before the declared body length. The scripted
// transport accepts the preface + HEADERS write; the read-error is hit before any response is read.
TEST(HttpClientHttp2E2ETest, PostFileBodyReadErrorFailsExchange) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, MakeLargeBody());
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);

  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, "http://example.test/upload")
                 .file(std::move(file), "application/octet-stream");
  HttpRequestTest::Finalize(req);
  // Truncate under us: the open fd now reads 0 bytes though the body length was 1 MiB.
  std::filesystem::resize_file(tmp.filePath(), 0);

  // CloseOnRead accepts all writes (preface + HEADERS) and never feeds a response; the exchange fails at
  // the first file read, before any transport read.
  ScriptedHttp2Transport transport(ScriptedHttp2Transport::Action::CloseOnRead);
  internal::ClientConnection connection(client.config().http2);
  bool requestSent = false;

  auto result = connection.exchange(client, transport, kInvalidHandle, req,
                                    SteadyClock::now() + std::chrono::seconds{1}, requestSent);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::writeError);
}

TEST(HttpClientHttp2E2ETest, LargeResponseBodyReassembledAcrossDataFrames) {
  // Decompression off => no Accept-Encoding => the server must ship the raw 1 MiB: it exceeds the
  // client's 65535-byte initial stream window, so the server defers behind flow control and resumes on
  // the client's automatic WINDOW_UPDATEs.
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2).withDecompression(false);
  HttpClient client(cfg);
  auto resp = client.get(Url("/big")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), MakeLargeBody());
}

TEST(HttpClientHttp2E2ETest, TransparentResponseDecompression) {
  // Default client: Accept-Encoding advertised, the (highly repetitive) 1 MiB body is compressed by the
  // server and transparently decoded by the client, dropping the Content-Encoding header.
  HttpClient client = MakeHttp2Client();
  auto resp = client.get(Url("/big")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), MakeLargeBody());
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());
}

TEST(HttpClientHttp2E2ETest, KeepAliveDisabledReconnectsPerRequest) {
  // Without keep-alive every exchange runs on a fresh connection: preface + SETTINGS each time, and the
  // engine (with its HPACK state) is dropped rather than pooled.
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2);
  cfg.keepAlive = false;
  HttpClient client(cfg);
  for (int reqIdx = 0; reqIdx < 3; ++reqIdx) {
    auto resp = client.get(Url("/hello")).value();
    ASSERT_EQ(resp.status(), 200);
    ASSERT_EQ(resp.bodyInMemory(), "world");
  }
  EXPECT_TRUE(lastSeenHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2E2ETest, ManySequentialRequestsOnOneConnection) {
  // Drives stream ids well past the closed-stream retention window (pruning), past the server's
  // SETTINGS_MAX_CONCURRENT_STREAMS default of 100 (all streams are sequential, so per-connection
  // active-stream accounting must drop back to zero after each exchange), and reuses the HPACK dynamic
  // tables across exchanges.
  HttpClient client = MakeHttp2Client();
  for (int reqIdx = 0; reqIdx < 150; ++reqIdx) {
    auto resp = client.get(Url("/hello")).value();
    ASSERT_EQ(resp.status(), 200);
    ASSERT_EQ(resp.bodyInMemory(), "world");
  }
}

TEST(HttpClientHttp2E2ETest, RedirectFollowed) {
  HttpClient client = MakeHttp2Client();
  auto resp = client.get(Url("/redirect")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST(HttpClientHttp2E2ETest, SeeOtherRewritesToGetAndDropsBody) {
  HttpClient client = MakeHttp2Client();
  auto resp = client.post(Url("/see-other"), "discard-me", "text/plain").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST(HttpClientHttp2E2ETest, HeadHasNoBody) {
  HttpClient client = MakeHttp2Client();
  auto resp = client.head(Url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST(HttpClientHttp2E2ETest, RequestBodyCompression) {
  // Opt-in outbound compression: the engine rewrites content-length to the compressed size and appends
  // content-encoding; the server transparently decompresses and echoes the original payload back.
  const std::string payload = MakeLargeBody();
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2).withRequestCompression(true);
  cfg.requestCompression.codec.minBytes = 16;
  HttpClient client(cfg);
  auto resp = client.post(Url("/echo"), payload, "application/octet-stream").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), payload);
}

TEST(HttpClientHttp2E2ETest, UserFramingHeadersRespected) {
  // A user-supplied Host becomes the :authority value, explicit User-Agent / Accept-Encoding suppress
  // the injected ones, and connection-specific headers (forbidden in HTTP/2) are silently dropped.
  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, Url("/echo"));
  req.header("Host", "override-authority.test")
      .header("User-Agent", "custom-agent/1.0")
      .header("Accept-Encoding", "identity")
      .header("Keep-Alive", "timeout=5")
      .header("Proxy-Connection", "keep-alive")
      .body("payload", "text/plain");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "payload");
}

TEST(HttpClientHttp2E2ETest, DropBodyRedirectKeepsUserHeader) {
  // A 303 rewrites POST -> GET and drops the body: the body's Content-Type is dropped from the rewritten
  // header block, but an unrelated user header survives and reaches the redirect target.
  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, Url("/see-other-headers"));
  req.header("x-custom-token", "kept-across-redirect").body("discard-me", "text/plain");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.headerValueOrEmpty("x-echoed"), "kept-across-redirect");
}

TEST(HttpClientHttp2E2ETest, ClientStreamBudgetLimitsConnectionReuse) {
  // With a 2-stream budget per connection the pooled connection is not reused for the 3rd request; the
  // client reconnects transparently and all requests succeed.
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2);
  cfg.http2.maxStreamsPerConnection = 2;
  HttpClient client(cfg);
  for (int reqIdx = 0; reqIdx < 3; ++reqIdx) {
    auto resp = client.get(Url("/hello")).value();
    ASSERT_EQ(resp.status(), 200);
    ASSERT_EQ(resp.bodyInMemory(), "world");
  }
}

TEST(HttpClientHttp2E2ETest, CustomHeadersRoundTrip) {
  HttpClient client = MakeHttp2Client();
  // Uppercase name on purpose: HTTP/2 requires lowercase field names on the wire, the engine lowers it.
  auto req = client.makeRequest(http::Method::GET, Url("/headers"));
  req.header("X-Custom-Token", "abc123");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.headerValueOrEmpty("x-echoed"), "abc123");
  EXPECT_EQ(resp.headerValueOrEmpty("x-server"), "aeronet");
}

TEST(HttpClientHttp2E2ETest, MaxResponseBytesEnforced) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2);
  // The cap applies to the received (wire) body: disable decompression so no Accept-Encoding is
  // advertised and the server cannot shrink the 1 MiB body below the cap by compressing it.
  cfg.withDecompression(false);
  cfg.maxResponseBytes = 1024;
  HttpClient client(cfg);
  auto res = client.get(Url("/big"));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), HttpClientErrc::malformedResponse);
}

TEST(HttpClientHttp2E2ETest, InvalidHttp2ConfigRejectedAtConstruction) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2);
  cfg.http2.maxFrameSize = 1;  // below the RFC 9113 minimum of 16384
  EXPECT_THROW(HttpClient{cfg}, std::invalid_argument);
}

// --- Request trailers (RFC 9113 §8.1) --------------------------------------
// Over HTTP/2 trailers ride in a trailing HEADERS block that carries END_STREAM (no chunked reframing as
// in HTTP/1.1). END_STREAM is withheld from the initial HEADERS / final DATA frame and delivered on the
// trailing block; the server surfaces the decoded fields through req.trailers().

TEST(HttpClientHttp2E2ETest, SendsSingleRequestTrailer) {
  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, Url("/trailer-echo"));
  req.body("payload-data", "text/plain").trailerAddLine("x-checksum", "abc123");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "payload-data");
  EXPECT_EQ(resp.headerValueOrEmpty("echo-checksum"), "abc123");
  EXPECT_EQ(resp.headerValueOrEmpty("echo-trailer-count"), "1");
  EXPECT_TRUE(lastSeenHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2E2ETest, SendsMultipleRequestTrailers) {
  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::PUT, Url("/trailer-echo"));
  req.body("second-payload", "text/plain")
      .trailerAddLine("x-checksum", "deadbeef")
      .trailerAddLine("x-signature", "sig-42");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "second-payload");
  EXPECT_EQ(resp.headerValueOrEmpty("echo-checksum"), "deadbeef");
  EXPECT_EQ(resp.headerValueOrEmpty("echo-signature"), "sig-42");
  EXPECT_EQ(resp.headerValueOrEmpty("echo-trailer-count"), "2");
}

// A trailer with an empty value is still transmitted (present with an empty value, distinct from absent).
TEST(HttpClientHttp2E2ETest, TrailerWithEmptyValue) {
  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, Url("/trailer-echo"));
  req.body("body", "text/plain").trailerAddLine("x-checksum", "");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.headerValueOrEmpty("echo-trailer-count"), "1");
  EXPECT_EQ(resp.headerValueOrEmpty("echo-checksum"), "");
}

// A 1 MiB body forces many DATA frames across several flow-control windows; END_STREAM must be withheld
// from every DATA frame and delivered only on the trailing HEADERS block after the whole body is shipped.
TEST(HttpClientHttp2E2ETest, LargeBodyThenTrailers) {
  const std::string payload = MakeLargeBody();
  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, Url("/trailer-echo"));
  req.body(payload, "application/octet-stream").trailerAddLine("x-checksum", "big-crc");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), payload);
  EXPECT_EQ(resp.headerValueOrEmpty("echo-checksum"), "big-crc");
  EXPECT_EQ(resp.headerValueOrEmpty("echo-trailer-count"), "1");
}

// The trailered request must leave the pooled connection reusable: a plain, trailerless request right
// after a trailered one (same client) must frame and parse cleanly, and must not inherit stale trailers.
TEST(HttpClientHttp2E2ETest, ReusableAfterTraileredRequest) {
  HttpClient client = MakeHttp2Client();
  auto first = client.makeRequest(http::Method::POST, Url("/trailer-echo"));
  first.body("with-trailer", "text/plain").trailerAddLine("x-checksum", "c1");
  auto r1 = client.request(first).value();
  EXPECT_EQ(r1.status(), 200);
  EXPECT_EQ(r1.headerValueOrEmpty("echo-checksum"), "c1");

  // A second, trailerless request frames as a normal END_STREAM-on-DATA body and carries no trailers.
  auto r2 = client.post(Url("/trailer-echo"), "no-trailer", "text/plain").value();
  EXPECT_EQ(r2.status(), 200);
  EXPECT_EQ(r2.bodyInMemory(), "no-trailer");
  EXPECT_EQ(r2.headerValueOrEmpty("echo-trailer-count"), "0");
}

TEST(HttpClientHttp2E2ETest, ServerGoAwayAfterStreamBudgetReconnects) {
  // The server GOAWAYs the connection after one stream: the client observes it (during the first
  // exchange or on the pooled-connection vetting), drops the connection and reconnects transparently.

  auto routerUpdateProxy = ts.resetRouterAndGet();
  routerUpdateProxy.setPath(http::Method::GET, "/hello", [](const HttpRequestView&) {
    return HttpResponse(http::StatusCodeOK, "world", "text/plain");
  });

  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.http2.maxStreamsPerConnection = 1; });

  HttpClient client = MakeHttp2Client();
  for (int reqIdx = 0; reqIdx < 3; ++reqIdx) {
    auto resp = client.get(Url("/hello")).value();
    ASSERT_EQ(resp.status(), 200);
    ASSERT_EQ(resp.bodyInMemory(), "world");
  }
}

#ifdef AERONET_ENABLE_OPENSSL

namespace {

// TLS server factory: h2 is offered through ALPN by default; `enableHttp2 = false` restricts the
// server to http/1.1 so the client's Http2 mode has nothing to negotiate.
std::unique_ptr<SingleHttpServer> MakeTlsServer(std::string_view cert, std::string_view key,
                                                std::atomic<bool>& sawHttp2, uint16_t& port, bool enableHttp2 = true) {
  // A client dropping its connection right after the handshake (the ALPN-mismatch test) can make the
  // in-process server's OpenSSL stack write to a closed fd, raising SIGPIPE on Linux -- same mitigation
  // as test_tls_client.cpp. Windows has no SIGPIPE concept.
#ifdef AERONET_POSIX
  std::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
#endif
  Router router;
  router.setPath(http::Method::GET, "/hello", [&sawHttp2](const HttpRequestView& req) {
    sawHttp2.store(req.version() == http::HTTP_2_0, std::memory_order_relaxed);
    return HttpResponse(http::StatusCodeOK, "tls-world", "text/plain");
  });
  HttpServerConfig cfg;
  cfg.withPort(0).withPollInterval(std::chrono::milliseconds{20}).withTlsCertKeyMemory(cert, key);
  cfg.http2.enable = enableHttp2;
  if (enableHttp2) {
    cfg.withTlsAlpnProtocols({"h2", "http/1.1"});  // offer h2 via ALPN (not advertised by default)
  }
  auto server = std::make_unique<SingleHttpServer>(std::move(cfg), std::move(router));
  port = server->port();
  server->start();
  return server;
}

HttpClientConfig TlsClientConfig(HttpVersionMode mode) {
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;  // ephemeral self-signed test certificate
  cfg.withHttpVersion(mode);
  return cfg;
}

std::string TlsUrl(uint16_t port) { return "https://localhost:" + std::to_string(port) + "/hello"; }

}  // namespace

TEST(HttpClientHttp2TlsE2ETest, AutoModeNegotiatesH2ViaAlpn) {
  auto [cert, key] = test::MakeEphemeralCertKey("localhost");
  std::atomic<bool> sawHttp2{false};
  uint16_t port{0};
  auto server = MakeTlsServer(cert, key, sawHttp2, port);
  HttpClient client(TlsClientConfig(HttpVersionMode::Auto));
  auto resp = client.get(TlsUrl(port)).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "tls-world");
  EXPECT_TRUE(sawHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2TlsE2ETest, Http2ModeNegotiatesH2ViaAlpn) {
  auto [cert, key] = test::MakeEphemeralCertKey("localhost");
  std::atomic<bool> sawHttp2{false};
  uint16_t port{0};
  auto server = MakeTlsServer(cert, key, sawHttp2, port);
  HttpClient client(TlsClientConfig(HttpVersionMode::Http2));
  auto resp = client.get(TlsUrl(port)).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(sawHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2TlsE2ETest, Http11ModeNeverNegotiatesH2) {
  auto [cert, key] = test::MakeEphemeralCertKey("localhost");
  std::atomic<bool> sawHttp2{true};
  uint16_t port{0};
  auto server = MakeTlsServer(cert, key, sawHttp2, port);
  HttpClient client(TlsClientConfig(HttpVersionMode::Http1_1));
  auto resp = client.get(TlsUrl(port)).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_FALSE(sawHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2TlsE2ETest, Http2ModeFailsWhenOriginHasNoH2) {
  auto [cert, key] = test::MakeEphemeralCertKey("localhost");
  std::atomic<bool> sawHttp2{false};
  uint16_t port{0};
  auto server = MakeTlsServer(cert, key, sawHttp2, port, /*enableHttp2=*/false);
  HttpClient client(TlsClientConfig(HttpVersionMode::Http2));
  auto res = client.get(TlsUrl(port));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), HttpClientErrc::protocolUnsupported);
}

#endif  // AERONET_ENABLE_OPENSSL

}  // namespace aeronet
