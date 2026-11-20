#include "aeronet/websocket-client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/close-native-handle.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/router.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/websocket-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-frame.hpp"
#include "aeronet/websocket-handler.hpp"
#include "aeronet/websocket-upgrade.hpp"
#include "raw-test-server.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/single-http-server.hpp"
#include "aeronet/test-tls-helper.hpp"
#include "aeronet/tls-config.hpp"
#endif

#ifdef AERONET_LINUX
// The epoll_ctl override (the socket calls pass through).
#define AERONET_WANT_SOCKET_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#endif

namespace aeronet {

// The internals of a WebSocketConnection that the tests reach.
class WebSocketConnectionTest {
 public:
  // Replace the transport of 'cnx', which keeps its socket.
  static void SetTransport(WebSocketConnection& cnx, Transport transport) { cnx._transport = std::move(transport); }

  [[nodiscard]] static NativeHandle Fd(const WebSocketConnection& cnx) { return cnx._cnx.fd(); }

  // The events that the event loop of the client watches for 'cnx'.
  [[nodiscard]] static EventBmp Interest(const WebSocketConnection& cnx) { return cnx._interest; }
};

namespace {

using namespace std::chrono_literals;
using websocket::CloseCode;

test::TestServer ts;

std::string_view AsText(std::span<const std::byte> payload) {
  return {reinterpret_cast<const char*>(payload.data()), payload.size()};
}

std::span<const std::byte> AsBytes(std::string_view text) { return std::as_bytes(std::span(text)); }

// What the callbacks of a connection received: the user data of the connection.
struct Events {
  std::vector<std::string> messages;
  std::vector<bool> binary;
  std::vector<std::string> pongs;
  std::string closeReason;
  WebSocketConnection* pForwardTo{nullptr};  // the connection to which onMessage forwards the messages, if any
  std::size_t bufferedAmountInOnClose{0};
  int nbOpen{0};
  int nbClose{0};
  CloseCode closeCode{CloseCode::Normal};
};

Events& EventsOf(WebSocketConnection& cnx) { return *static_cast<Events*>(cnx.userData()); }

// Records the events of a connection in its user data, an Events.
constexpr WebSocketClientCallbacks kRecording{
    .onOpen = [](WebSocketConnection& cnx) { ++EventsOf(cnx).nbOpen; },
    .onMessage =
        [](WebSocketConnection& cnx, std::span<const std::byte> payload, bool isBinary) {
          Events& events = EventsOf(cnx);
          events.messages.emplace_back(AsText(payload));
          events.binary.push_back(isBinary);
        },
    .onPong = [](WebSocketConnection& cnx,
                 std::span<const std::byte> payload) { EventsOf(cnx).pongs.emplace_back(AsText(payload)); },
    .onClose =
        [](WebSocketConnection& cnx, CloseCode code, std::string_view reason) {
          Events& events = EventsOf(cnx);
          ++events.nbClose;
          events.closeCode = code;
          events.closeReason = reason;
          events.bufferedAmountInOnClose = cnx.bufferedAmount();
          // Nothing can be sent anymore, whatever ended the connection.
          EXPECT_FALSE(cnx.isOpen());
          EXPECT_FALSE(cnx.sendText("after the end"));
          EXPECT_FALSE(cnx.sendPing());
          EXPECT_FALSE(cnx.close());
        },
};

// Poll 'client' until 'done' holds, for 'timeout' at most. Returns done().
bool PollUntil(WebSocketClient& client, const std::function<bool()>& done, std::chrono::milliseconds timeout = 10s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    client.poll(10ms);
  }
  return true;
}

// Server handler of the /echo endpoint: echoes the messages, except the commands of the text ones.
std::unique_ptr<websocket::WebSocketHandler> MakeEchoHandler(const HttpRequestView& /*request*/) {
  auto handler = std::make_unique<websocket::WebSocketHandler>();
  websocket::WebSocketHandler* pHandler = handler.get();  // valid as long as the connection lives
  handler->setCallbacks({
      .onMessage =
          [pHandler](std::span<const std::byte> payload, bool isBinary) {
            const std::string_view text = AsText(payload);
            if (isBinary) {
              pHandler->sendBinary(payload);
            } else if (text.starts_with("close:")) {
              pHandler->sendClose(static_cast<CloseCode>(std::stoi(std::string(text.substr(6)))), "server bye");
            } else if (text == "ping") {
              pHandler->sendPing(AsBytes("srv"));
            } else if (text.starts_with("burst:")) {
              const int nbMessages = std::stoi(std::string(text.substr(6)));
              for (int idx = 0; idx < nbMessages; ++idx) {
                pHandler->sendText("m" + std::to_string(idx));
              }
            } else {
              pHandler->sendText(text);
            }
          },
      .onPing = {},
      // The Pong answering the Ping of the "ping" command, sent back as a message.
      .onPong = [pHandler](
                    std::span<const std::byte> payload) { pHandler->sendText("pong:" + std::string(AsText(payload))); },
      .onClose = {},
      .onError = {},
  });
  return handler;
}

void SetEchoRoutes(Router& router) {
  WebSocketEndpoint echo = WebSocketEndpoint::WithFactory(MakeEchoHandler);
  echo.supportedProtocols.append("v2");
  router.setWebSocket("/echo", std::move(echo));
  router.setWebSocket("/forbidden",
                      WebSocketEndpoint::WithFactory([](const HttpRequestView& /*request*/) { return nullptr; }));
  router.setPath(http::Method::GET, "/plain", [](const HttpRequestView& /*request*/) {
    return HttpResponse(http::StatusCodeOK, "not a websocket", "text/plain");
  });
}

// A client configuration without compression, for the tests that do not need it.
websocket::WebSocketConfig PlainConfig() {
  websocket::WebSocketConfig config;
#ifdef AERONET_ENABLE_ZLIB
  config.deflateConfig.enabled = false;
#endif
  return config;
}

class WebSocketClientTest : public ::testing::Test {
 protected:
  void SetUp() override { ts.postRouterUpdate(SetEchoRoutes); }

  [[nodiscard]] static std::string url(std::string_view path, std::string_view scheme = "ws") {
    return std::string(scheme) + "://127.0.0.1:" + std::to_string(ts.port()) + std::string(path);
  }
};

TEST_F(WebSocketClientTest, ConnectsSendsFromOnOpenAndCloses) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketClientCallbacks callbacks = kRecording;
  callbacks.onOpen = [](WebSocketConnection& cnx) {
    ++EventsOf(cnx).nbOpen;
    EXPECT_TRUE(cnx.sendText("hello"));
  };
  WebSocketConnectResult result = client.connect(url("/echo"), callbacks, &events);
  ASSERT_TRUE(result) << result.pErrorMessage;
  EXPECT_EQ(result.response.status(), http::StatusCodeSwitchingProtocols);
  EXPECT_EQ(result.pErrorMessage, nullptr);
  EXPECT_EQ(events.nbOpen, 1);
  WebSocketConnection& cnx = *result.pConnection;
  EXPECT_EQ(&cnx.client(), &client);
  EXPECT_EQ(cnx.userData(), &events);
  EXPECT_TRUE(cnx.isOpen());
  EXPECT_EQ(client.nbConnections(), 1U);

  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; }));
  EXPECT_EQ(events.messages[0], "hello");
  EXPECT_FALSE(events.binary[0]);

  EXPECT_TRUE(cnx.close(CloseCode::Normal, "done"));
  EXPECT_FALSE(cnx.isOpen());
  EXPECT_FALSE(cnx.sendText("too late"));
  EXPECT_FALSE(cnx.close());
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.closeCode, CloseCode::Normal);
  EXPECT_EQ(events.closeReason, "done");
  EXPECT_EQ(client.poll(0ms), 0U);
}

TEST_F(WebSocketClientTest, EchoesBinaryMessagesSentOutOfTheCallbacks) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  static constexpr std::array<std::byte, 4> kData{std::byte{0}, std::byte{1}, std::byte{0xFE}, std::byte{0xFF}};
  EXPECT_TRUE(result.pConnection->sendBinary(kData));
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; }));
  EXPECT_TRUE(events.binary[0]);
  EXPECT_EQ(events.messages[0], std::string_view(reinterpret_cast<const char*>(kData.data()), kData.size()));
}

TEST_F(WebSocketClientTest, AcceptsTheWebSocketAndHttpSchemes) {
  for (const std::string_view scheme : {"ws", "WS", "http"}) {
    WebSocketClient client(PlainConfig());
    Events events;
    WebSocketConnectResult result = client.connect(url("/echo", scheme), kRecording, &events);
    ASSERT_TRUE(result) << scheme;
    EXPECT_TRUE(result.pConnection->sendText(scheme));
    ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; })) << scheme;
    EXPECT_EQ(events.messages[0], scheme);
  }
}

TEST_F(WebSocketClientTest, RefusedUpgradeGivesTheResponse) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult plain = client.connect(url("/plain"), kRecording, &events);
  EXPECT_FALSE(plain);
  EXPECT_EQ(plain.pConnection, nullptr);
  EXPECT_EQ(plain.httpError, HttpClientErrc::noError);
  EXPECT_STREQ(plain.pErrorMessage, "the server did not switch to the WebSocket protocol");
  EXPECT_EQ(plain.response.status(), http::StatusCodeOK);
  EXPECT_EQ(plain.response.bodyInMemory(), "not a websocket");

  WebSocketConnectResult forbidden = client.connect(url("/forbidden"), kRecording, &events);
  EXPECT_FALSE(forbidden);
  EXPECT_EQ(forbidden.response.status(), http::StatusCodeForbidden);
  EXPECT_EQ(events.nbOpen, 0);
  EXPECT_EQ(client.nbConnections(), 0U);
}

TEST_F(WebSocketClientTest, TransportFailureGivesTheHttpError) {
  // A port nobody listens on: the one of a raw server just stopped.
  uint16_t closedPort;
  {
    const test::RawServer server([](NativeHandle /*fd*/, int /*connIndex*/) {});
    closedPort = server.port();
  }
  HttpClientConfig httpConfig;
  httpConfig.connectTimeout = 1s;
  WebSocketClient client(PlainConfig(), httpConfig);
  Events events;
  WebSocketConnectResult result =
      client.connect("ws://127.0.0.1:" + std::to_string(closedPort) + "/echo", kRecording, &events);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.httpError, HttpClientErrc::connectFailed);
  EXPECT_EQ(std::string_view(result.pErrorMessage), ErrcToStr(HttpClientErrc::connectFailed));
}

TEST_F(WebSocketClientTest, InvalidUrlThrows) {
  WebSocketClient client;
  EXPECT_THROW(static_cast<void>(client.connect("ftp://127.0.0.1/echo", {})), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(client.connect("ws:/127.0.0.1/echo", {})), std::invalid_argument);
}

TEST_F(WebSocketClientTest, ServerStartsTheClosingHandshake) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.pConnection->sendText("close:4000"));
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.closeCode, static_cast<CloseCode>(4000));
  EXPECT_EQ(events.closeReason, "server bye");
}

TEST_F(WebSocketClientTest, NegotiatesAnOfferedSubprotocol) {
  WebSocketClient client(PlainConfig());
  HttpRequest req = client.makeUpgradeRequest(url("/echo"));
  req.headerAddLine(websocket::SecWebSocketProtocol, "chat, v2");
  Events events;
  WebSocketConnectResult result = client.connect(std::move(req), kRecording, &events);
  ASSERT_TRUE(result) << result.pErrorMessage;
  EXPECT_EQ(result.response.headerValueOrEmpty(websocket::SecWebSocketProtocol), "v2");
}

TEST_F(WebSocketClientTest, PingIsAnsweredByAPong) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.pConnection->sendPing(AsBytes("abc")));
  ASSERT_TRUE(PollUntil(client, [&events] { return events.pongs.size() == 1; }));
  EXPECT_EQ(events.pongs[0], "abc");
}

TEST_F(WebSocketClientTest, PingOfTheServerIsAnswered) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.pConnection->sendText("ping"));
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; }));
  EXPECT_EQ(events.messages[0], "pong:srv");
}

#ifdef AERONET_LINUX
// Timing: a process descheduled for longer than the idle timeout (macOS runners oversleep by about 1 s) would end it.
TEST_F(WebSocketClientTest, AutomaticPingsKeepAnIdleConnectionOpen) {
  websocket::WebSocketConfig config = PlainConfig();
  config.idleTimeout = 200ms;
  WebSocketClient client(config);
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  // Three idle timeouts: the Pongs answering the automatic Pings keep the connection open.
  const auto end = std::chrono::steady_clock::now() + 3 * config.idleTimeout;
  while (std::chrono::steady_clock::now() < end) {
    client.poll(50ms);
  }
  EXPECT_EQ(events.nbClose, 0);
  EXPECT_FALSE(events.pongs.empty());
  EXPECT_TRUE(result.pConnection->isOpen());
}
#endif

TEST_F(WebSocketClientTest, ManySmallMessagesInARow) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  static constexpr int kNbMessages = 20000;
  EXPECT_TRUE(result.pConnection->sendText("burst:" + std::to_string(kNbMessages)));
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == kNbMessages; }));
  for (int idx = 0; idx < kNbMessages; ++idx) {
    ASSERT_EQ(events.messages[static_cast<std::size_t>(idx)], "m" + std::to_string(idx));
  }
}

TEST_F(WebSocketClientTest, LargeMessagesBothWays) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  // Larger than the socket buffers: written as the server reads it, then received in many reads.
  std::string big(8UL * 1024UL * 1024UL, 'x');
  for (std::size_t pos = 0; pos < big.size(); pos += 4096) {
    big[pos] = static_cast<char>('a' + ((pos / 4096) % 26));
  }
  EXPECT_TRUE(result.pConnection->sendBinary(AsBytes(big)));
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; }, 30s));
  EXPECT_TRUE(events.binary[0]);
  EXPECT_EQ(events.messages[0], big);
  EXPECT_EQ(result.pConnection->bufferedAmount(), 0U);
}

TEST_F(WebSocketClientTest, CallbacksSendOnAnotherConnection) {
  WebSocketClient client(PlainConfig());
  Events eventsA;
  Events eventsB;
  WebSocketConnectResult resultB = client.connect(url("/echo"), kRecording, &eventsB);
  ASSERT_TRUE(resultB);
  WebSocketConnection* pB = resultB.pConnection;
  WebSocketClientCallbacks callbacksA = kRecording;
  callbacksA.onMessage = [](WebSocketConnection& cnx, std::span<const std::byte> payload, bool /*isBinary*/) {
    Events& events = EventsOf(cnx);
    events.messages.emplace_back(AsText(payload));
    EXPECT_TRUE(events.pForwardTo->sendText("from A: " + std::string(AsText(payload))));
  };
  eventsA.pForwardTo = pB;
  WebSocketConnectResult resultA = client.connect(url("/echo"), callbacksA, &eventsA);
  ASSERT_TRUE(resultA);
  EXPECT_EQ(client.nbConnections(), 2U);
  EXPECT_TRUE(resultA.pConnection->sendText("one"));
  ASSERT_TRUE(PollUntil(client, [&eventsB] { return eventsB.messages.size() == 1; }));
  EXPECT_EQ(eventsA.messages, std::vector<std::string>{"one"});
  EXPECT_EQ(eventsB.messages[0], "from A: one");
}

// Poll 'client' until a callback throws a std::runtime_error, for 10 s at most. Returns whether one did.
bool PollUntilACallbackThrows(WebSocketClient& client) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      client.poll(10ms);
    } catch (const std::runtime_error&) {
      return true;
    }
  }
  return false;
}

TEST_F(WebSocketClientTest, ThrowingCallbackEndsItsConnection) {
  WebSocketClient client(PlainConfig());
  Events eventsA;
  Events eventsB;
  WebSocketClientCallbacks throwing = kRecording;
  throwing.onMessage = [](WebSocketConnection& cnx, std::span<const std::byte> payload, bool /*isBinary*/) {
    EventsOf(cnx).messages.emplace_back(AsText(payload));
    if (AsText(payload) == "m1") {
      throw std::runtime_error("m1");
    }
  };
  WebSocketConnectResult resultA = client.connect(url("/echo"), throwing, &eventsA);
  ASSERT_TRUE(resultA);
  WebSocketConnectResult resultB = client.connect(url("/echo"), kRecording, &eventsB);
  ASSERT_TRUE(resultB);
  // Three messages sent together: the one after the throwing callback is never delivered, and none twice.
  EXPECT_TRUE(resultA.pConnection->sendText("burst:3"));
  ASSERT_TRUE(PollUntilACallbackThrows(client));
  EXPECT_EQ(client.nbConnections(), 1U);  // destroyed at once, without onClose
  EXPECT_EQ(eventsA.messages, (std::vector<std::string>{"m0", "m1"}));
  EXPECT_EQ(eventsA.nbClose, 0);

  // The other connection carries on.
  EXPECT_TRUE(resultB.pConnection->sendText("still open"));
  ASSERT_TRUE(PollUntil(client, [&eventsB] { return eventsB.messages.size() == 1; }));
  EXPECT_EQ(eventsB.messages[0], "still open");
  EXPECT_EQ(eventsA.messages.size(), 2U);
}

TEST_F(WebSocketClientTest, ThrowingOnOpenEndsTheConnection) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketClientCallbacks callbacks = kRecording;
  callbacks.onOpen = [](WebSocketConnection& cnx) {
    ++EventsOf(cnx).nbOpen;
    throw std::runtime_error("onOpen");
  };
  EXPECT_THROW(static_cast<void>(client.connect(url("/echo"), callbacks, &events)), std::runtime_error);
  EXPECT_EQ(events.nbOpen, 1);
  EXPECT_EQ(client.nbConnections(), 0U);
  EXPECT_EQ(client.poll(0ms), 0U);
  EXPECT_EQ(events.nbClose, 0);
}

TEST_F(WebSocketClientTest, MovedClientKeepsItsConnections) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  WebSocketClient moved(std::move(client));
  EXPECT_EQ(&result.pConnection->client(), &moved);
  WebSocketClient assigned;
  assigned = std::move(moved);
  auto& self = assigned;
  assigned = std::move(self);  // no effect
  EXPECT_EQ(&result.pConnection->client(), &assigned);
  EXPECT_EQ(assigned.nbConnections(), 1U);
  EXPECT_TRUE(result.pConnection->sendText("after move"));
  ASSERT_TRUE(PollUntil(assigned, [&events] { return events.messages.size() == 1; }));
  EXPECT_EQ(events.messages[0], "after move");
  EXPECT_FALSE(assigned.config().isServerSide);
  EXPECT_EQ(assigned.httpClient().config().connectTimeout, HttpClientConfig{}.connectTimeout);
}

TEST_F(WebSocketClientTest, ClientWithoutConnectionWaitsTheTimeout) {
  WebSocketClient client;
  const auto before = std::chrono::steady_clock::now();
  EXPECT_EQ(client.poll(20ms), 0U);
  EXPECT_GE(std::chrono::steady_clock::now() - before, 20ms);
}

TEST_F(WebSocketClientTest, InvalidConfigurationThrows) {
  websocket::WebSocketConfig config;
  config.maxMessageSize = 0;
  EXPECT_THROW(static_cast<void>(WebSocketClient(config)), std::invalid_argument);
}

#ifdef AERONET_ENABLE_ZLIB
TEST_F(WebSocketClientTest, CompressesBothWays) {
  WebSocketClient client;  // permessage-deflate offered by default
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.response.headerValueOrEmpty(websocket::SecWebSocketExtensions).starts_with("permessage-deflate"));
  // A compressible message, and one compressed into more than the buffered output (sent from the compression buffer).
  const std::string compressible(10000, 'z');
  std::string lessCompressible;
  uint32_t state = 12345;
  for (int idx = 0; idx < 200000; ++idx) {
    state = (state * 1103515245U) + 12345U;
    lessCompressible.push_back(static_cast<char>('a' + ((state >> 16U) % 16U)));
  }
  EXPECT_TRUE(result.pConnection->sendText(compressible));
  EXPECT_TRUE(result.pConnection->sendText(lessCompressible));
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 2; }));
  EXPECT_EQ(events.messages[0], compressible);
  EXPECT_EQ(events.messages[1], lessCompressible);
}
#endif

#ifdef AERONET_ENABLE_OPENSSL
TEST(WebSocketClientTls, EchoesOverTlsWithAlpnOfferingHttp11Only) {
  auto [certPem, keyPem] = test::MakeEphemeralCertKey("localhost");
  HttpServerConfig serverConfig;
  serverConfig.withPort(0).withPollInterval(std::chrono::milliseconds{5});
  serverConfig.tls.enabled = true;
  serverConfig.tls.withCertPem(certPem).withKeyPem(keyPem);
  // The server prefers h2: the upgrade connection only offers http/1.1.
  serverConfig.tls.withTlsAlpnProtocols(std::array<std::string_view, 2>{"h2", "http/1.1"});
  Router router;
  SetEchoRoutes(router);
  SingleHttpServer server(std::move(serverConfig), std::move(router));
  server.start();

  HttpClientConfig httpConfig;
  httpConfig.tlsVerifyPeer = false;
  WebSocketClient client(PlainConfig(), httpConfig);
  Events events;
  WebSocketConnectResult result =
      client.connect("wss://127.0.0.1:" + std::to_string(server.port()) + "/echo", kRecording, &events);
  ASSERT_TRUE(result) << result.pErrorMessage;
  EXPECT_TRUE(result.pConnection->sendText("over tls"));
  EXPECT_TRUE(result.pConnection->sendText("burst:3000"));
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 3001; }));
  EXPECT_EQ(events.messages[0], "over tls");
  EXPECT_EQ(events.messages[3000], "m2999");
  EXPECT_TRUE(result.pConnection->close());
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.closeCode, CloseCode::Normal);
}
#endif

// --- Scripted servers: the bytes of the server, byte for byte ---

// The 101 response to the upgrade request read from 'fd', with 'extraHeaders' (complete lines, before its Connection
// line). Returns the request.
std::string AnswerUpgrade(NativeHandle fd, std::string_view extraHeaders = {}, std::string_view after = {}) {
  std::string head = test::RecvHead(fd);
  static constexpr std::string_view kKeyLine = "sec-websocket-key: ";
  const std::string lower = test::toLower(head);
  const std::size_t keyPos = lower.find(kKeyLine) + kKeyLine.size();
  const B64EncodedSha1 accept = ComputeWebSocketAccept(std::string_view(head).substr(keyPos, 24));
  std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n";
  response += extraHeaders;
  response += "Connection: Upgrade\r\n";
  response += "Sec-WebSocket-Accept: " + std::string(accept.data(), accept.size()) + "\r\n";
  response += "\r\n";
  response += after;
  test::sendAll(fd, response);
  return head;
}

// An unmasked frame of the server.
std::string ServerFrame(websocket::Opcode opcode, std::string_view payload) {
  RawBytes frame;
  websocket::BuildFrame(frame, opcode, AsBytes(payload), true, false, {}, false);
  return {reinterpret_cast<const char*>(frame.data()), frame.size()};
}

// A masked frame of the server, which the client must reject (RFC 6455 §5.1).
std::string MaskedServerFrame() {
  std::string masked = ServerFrame(websocket::Opcode::Text, "abcd");
  masked[1] = static_cast<char>(masked[1] | 0x80);  // mask bit, then the masking key and the payload
  masked.insert(2, std::string(4, '\0'));
  return masked;
}

std::string RawUrl(const test::RawServer& server) { return "ws://127.0.0.1:" + std::to_string(server.port()) + "/"; }

// Keeps a scripted connection open until the test is done with it.
struct Hold {
  void release() { done.store(true); }

  void wait() const {
    while (!done.load()) {
      std::this_thread::sleep_for(5ms);
    }
  }

  std::atomic<bool> done{false};
};

// Releases a Hold when destroyed: declared after the server, so that a failed assertion does not leave its thread
// waiting forever in the destructor of the server.
class ReleaseAtExit {
 public:
  explicit ReleaseAtExit(std::shared_ptr<Hold> hold) noexcept : _hold(std::move(hold)) {}

  ReleaseAtExit(const ReleaseAtExit&) = delete;
  ReleaseAtExit(ReleaseAtExit&&) = delete;
  ReleaseAtExit& operator=(const ReleaseAtExit&) = delete;
  ReleaseAtExit& operator=(ReleaseAtExit&&) = delete;

  ~ReleaseAtExit() { _hold->release(); }

 private:
  std::shared_ptr<Hold> _hold;
};

TEST(WebSocketClientScripted, FrameSentWithThe101IsDelivered) {
  auto hold = std::make_shared<Hold>();
  const std::string early = ServerFrame(websocket::Opcode::Text, "early");
  test::RawServer server([hold, early](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd, {}, early);
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(result) << result.pErrorMessage;
  EXPECT_TRUE(events.messages.empty());  // delivered by poll()
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; }));
  EXPECT_EQ(events.messages[0], "early");
}

TEST(WebSocketClientScripted, DataAfterTheCloseFrameIsDiscarded) {
  // A Close frame followed in the same read by a text frame and an invalid (masked) one, then a text frame in a later
  // read. The server keeps the connection open: the client closes it after closeTimeout.
  const std::string bytes = ServerFrame(websocket::Opcode::Close, "\x03\xe8") +
                            ServerFrame(websocket::Opcode::Text, "same read") + MaskedServerFrame();
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold, bytes](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd);
    test::sendAll(fd, bytes);
    std::this_thread::sleep_for(30ms);
    test::sendAll(fd, ServerFrame(websocket::Opcode::Text, "later"));
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  websocket::WebSocketConfig config = PlainConfig();
  config.closeTimeout = 100ms;
  WebSocketClient client(config);
  Events events;
  ASSERT_TRUE(client.connect(RawUrl(server), kRecording, &events));
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.closeCode, CloseCode::Normal);
  EXPECT_TRUE(events.messages.empty());
}

TEST(WebSocketClientScripted, ServerClosingTheConnectionIsAnAbnormalClosure) {
  test::RawServer server([](NativeHandle fd, int /*connIndex*/) { AnswerUpgrade(fd); });
  WebSocketClient client(PlainConfig());
  Events events;
  ASSERT_TRUE(client.connect(RawUrl(server), kRecording, &events));
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.closeCode, CloseCode::AbnormalClosure);
  EXPECT_EQ(events.closeReason, "connection closed by the server");
}

TEST(WebSocketClientScripted, MaskedServerFrameFailsTheConnection) {
  // Received with the 101 response, then in a later read.
  for (const bool withThe101 : {true, false}) {
    auto received = std::make_shared<std::string>();
    auto done = std::make_shared<Hold>();
    test::RawServer server([done, received, withThe101](NativeHandle fd, int /*connIndex*/) {
      if (withThe101) {
        AnswerUpgrade(fd, {}, MaskedServerFrame());
      } else {
        AnswerUpgrade(fd);
        std::this_thread::sleep_for(20ms);
        test::sendAll(fd, MaskedServerFrame());
      }
      *received = test::recvUntilClosed(fd, 5s);  // the Close frame of the client, then its closing
      done->release();
    });
    const ReleaseAtExit releaseAtExit(done);
    WebSocketClient client(PlainConfig());
    Events events;
    ASSERT_TRUE(client.connect(RawUrl(server), kRecording, &events));
    ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; })) << withThe101;
    EXPECT_EQ(events.closeCode, CloseCode::ProtocolError) << withThe101;
    EXPECT_TRUE(events.messages.empty());
    done->wait();
    // A masked Close frame (opcode 8) with code 1002.
    ASSERT_GE(received->size(), 8U);
    EXPECT_EQ(static_cast<uint8_t>((*received)[0]), 0x88);
  }
}

TEST(WebSocketClientScripted, IdleTimeoutEndsTheConnection) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd);
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  websocket::WebSocketConfig config = PlainConfig();
  config.idleTimeout = 150ms;
  config.autoPing = false;
  WebSocketClient client(config);
  Events events;
  ASSERT_TRUE(client.connect(RawUrl(server), kRecording, &events));
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.closeCode, CloseCode::AbnormalClosure);
  EXPECT_EQ(events.closeReason, "idle timeout");
}

TEST(WebSocketClientScripted, UnansweredCloseFrameEndsTheConnectionAfterCloseTimeout) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd);
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  websocket::WebSocketConfig config = PlainConfig();
  config.closeTimeout = 100ms;
  WebSocketClient client(config);
  Events events;
  WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.pConnection->close(CloseCode::GoingAway));
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.closeCode, CloseCode::AbnormalClosure);
  EXPECT_EQ(events.closeReason, "no Close frame from the server within closeTimeout");
}

#ifdef AERONET_LINUX
// Socket buffers: Windows accepts the whole message, macOS has 8 KiB receive buffers (see the flow-control tests).
TEST(WebSocketClientScripted, BlockedWritesAreFlushedWhenTheSocketIsWritable) {
  // Larger than the socket buffers. The frames: masked binary one (64 bits length), masked text one of 13 bytes.
  static constexpr std::size_t kPayloadSize = 32UL * 1024UL * 1024UL;
  static constexpr std::size_t kWireSize = (14U + kPayloadSize) + (6U + 13U);
  auto start = std::make_shared<Hold>();
  auto nbReceived = std::make_shared<std::atomic<std::size_t>>(0);
  test::RawServer server([start, nbReceived](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd);
    start->wait();  // not read until the client has buffered output
    nbReceived->store(test::ReceiveAndDiscard(fd, kWireSize, 20s));
  });
  const ReleaseAtExit releaseAtExit(start);
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(result);
  const std::string big(kPayloadSize, 'b');
  EXPECT_TRUE(result.pConnection->sendBinary(AsBytes(big)));
  EXPECT_GT(result.pConnection->bufferedAmount(), 0U);
  EXPECT_TRUE(result.pConnection->sendText("queued behind"));  // not written before the socket is writable again
  start->release();
  ASSERT_TRUE(PollUntil(client, [&result] { return result.pConnection->bufferedAmount() == 0; }, 30s));
  EXPECT_EQ(events.nbClose, 0);
  result.pConnection->close();
  ASSERT_TRUE(PollUntil(client, [nbReceived] { return nbReceived->load() != 0; }, 30s));
  EXPECT_GE(nbReceived->load(), kWireSize);
}
#endif

#ifdef AERONET_LINUX
// Socket buffers, see BlockedWritesAreFlushedWhenTheSocketIsWritable.
TEST(WebSocketClientScripted, ResetWhileWritesAreBlockedEndsTheConnection) {
  auto blocked = std::make_shared<Hold>();
  test::RawServer server([blocked](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd);
    blocked->wait();  // then closed with unread bytes: reset
  });
  const ReleaseAtExit releaseAtExit(blocked);
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.pConnection->sendBinary(AsBytes(std::string(32UL * 1024UL * 1024UL, 'r'))));
  EXPECT_GT(result.pConnection->bufferedAmount(), 0U);
  blocked->release();
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.closeCode, CloseCode::AbnormalClosure);
  EXPECT_EQ(events.bufferedAmountInOnClose, 0U);  // the output of a lost connection is dropped
}
#endif

TEST(WebSocketClientScripted, SendOnAResetConnectionIsReportedByPoll) {
  test::RawServer server([](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd);  // then closes the connection
  });
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(result);
  std::this_thread::sleep_for(100ms);  // the server closed the connection
  // The first write is accepted and answered by a reset, the next ones fail: poll() then tells how it ended.
  for (int idx = 0; idx < 10 && events.nbClose == 0; ++idx) {
    result.pConnection->sendText("into the void");
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(events.nbClose, 0);  // never called from a send
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.closeCode, CloseCode::AbnormalClosure);
}

TEST(WebSocketClientScripted, HeadersSplitOverSeveralLinesAreCombined) {
  // Connection with a first line without upgrade, and the selected subprotocol offered on the second line of the
  // request.
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd, "Connection: keep-alive\r\nSec-WebSocket-Protocol: v2\r\n");
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig());
  HttpRequest req = client.makeUpgradeRequest(RawUrl(server));
  req.headerAddLine(websocket::SecWebSocketProtocol, "v1");
  req.headerAddLine(websocket::SecWebSocketProtocol, "v2");
  Events events;
  WebSocketConnectResult result = client.connect(std::move(req), kRecording, &events);
  ASSERT_TRUE(result) << result.pErrorMessage;
  EXPECT_EQ(events.nbOpen, 1);
}

struct InvalidHandshakeCase {
  std::string_view name;
  std::string_view headers;           // replacing or completing the valid 101 response
  std::string_view offeredProtocols;  // the Sec-WebSocket-Protocol of the request, none if empty
  std::string_view expectedError;
  bool validAccept{true};
  bool offerDeflate{false};
};

TEST(WebSocketClientScripted, InvalidHandshakeResponsesFailTheConnection) {
  static constexpr std::string_view kBadProtocol =
      "Sec-WebSocket-Protocol of the response is not one of the offered subprotocols";
  static constexpr std::string_view kBadExtension =
      "Sec-WebSocket-Extensions of the response holds an extension that was not offered";
  static constexpr InvalidHandshakeCase kCases[]{
      {"wrong accept", {}, {}, "Sec-WebSocket-Accept of the response does not match the key of the request", false},
      {"protocol not offered", "Sec-WebSocket-Protocol: chat\r\n", {}, kBadProtocol},
      // RFC 6455 §4.1: one subprotocol at most, and only offered extensions, whatever the lines they are on.
      {"second protocol line", "Sec-WebSocket-Protocol: v2\r\nSec-WebSocket-Protocol: x\r\n", "v2", kBadProtocol},
      {"extension not offered", "Sec-WebSocket-Extensions: x-unknown\r\n", {}, kBadExtension},
#ifdef AERONET_ENABLE_ZLIB
      {"second extensions line",
       "Sec-WebSocket-Extensions: permessage-deflate\r\nSec-WebSocket-Extensions: x-unknown\r\n",
       {},
       kBadExtension,
       true,
       true},
      {"invalid deflate parameters",
       "Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=16\r\n",
       {},
       "Sec-WebSocket-Extensions of the response holds invalid permessage-deflate parameters",
       true,
       true},
#endif
  };
  for (const InvalidHandshakeCase& testCase : kCases) {
    test::RawServer server([testCase](NativeHandle fd, int /*connIndex*/) {
      if (testCase.validAccept) {
        AnswerUpgrade(fd, testCase.headers);
      } else {
        test::RecvHead(fd);
        test::sendAll(fd,
                      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n");
      }
    });
    WebSocketClient client(testCase.offerDeflate ? websocket::WebSocketConfig{} : PlainConfig());
    HttpRequest req = client.makeUpgradeRequest(RawUrl(server));
    if (!testCase.offeredProtocols.empty()) {
      req.headerAddLine(websocket::SecWebSocketProtocol, testCase.offeredProtocols);
    }
    Events events;
    WebSocketConnectResult result = client.connect(std::move(req), kRecording, &events);
    EXPECT_FALSE(result) << testCase.name;
    EXPECT_EQ(result.response.status(), http::StatusCodeSwitchingProtocols) << testCase.name;
    EXPECT_EQ(std::string_view(result.pErrorMessage), testCase.expectedError) << testCase.name;
    EXPECT_EQ(events.nbOpen, 0) << testCase.name;
    EXPECT_EQ(client.nbConnections(), 0U) << testCase.name;
  }
}

#ifdef AERONET_LINUX
TEST(WebSocketClientScripted, EventLoopFailures) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int connIndex) {
    AnswerUpgrade(fd);
    if (connIndex != 0) {
      hold->wait();  // the first connection is closed by the client
    }
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig());
  Events events;
  // The handshake registers the socket in the event loop of the HttpClient, then the connection in the one of the
  // WebSocketClient: the second registration fails.
  test::PushEpollCtlAddAction({0, 0});
  test::PushEpollCtlAddAction({-1, ENOMEM});
  WebSocketConnectResult failed = client.connect(RawUrl(server), kRecording, &events);
  test::ResetEpollHooks();
  EXPECT_FALSE(failed);
  EXPECT_EQ(failed.httpError, HttpClientErrc::ioError);
  EXPECT_EQ(client.nbConnections(), 0U);

  // A write blocked by the socket needs a watch of its writability, which fails.
  WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(result);
  test::FailAllEpollCtlMod(ENOMEM);
  EXPECT_TRUE(result.pConnection->sendBinary(AsBytes(std::string(32UL * 1024UL * 1024UL, 'f'))));
  test::ResetEpollHooks();
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.closeCode, CloseCode::AbnormalClosure);
}
#endif

// The events watched for a connection, and with the writability of its socket.
constexpr EventBmp kReadEvents = EventIn | EventRdHup;
constexpr EventBmp kReadWriteEvents = kReadEvents | EventOut;

// A plain transport whose first reads follow a script, like TLS: each step reads 'holdBytes' bytes of the socket (a
// record), kept for the next reads, and returns no byte with 'hint'. WriteReady: TLS has to write before it reads
// again, like the answer to a TLS 1.3 KeyUpdate while the socket takes no more bytes. The bytes held are returned
// first by the reads after the script.
class ScriptedReadTransport final : public TransportBackend<ScriptedReadTransport> {
 public:
  struct Step {
    TransportHint hint;
    std::size_t holdBytes;
  };

  ScriptedReadTransport(NativeHandle fd, std::vector<Step> steps) : _plain(fd), _steps(std::move(steps)) {}

  TransportResult read(char* buf, std::size_t len) {
    if (_nextStep < _steps.size()) {
      const Step step = _steps[_nextStep++];
      std::array<char, 256> record;
      for (std::size_t nbHeld = 0; nbHeld < step.holdBytes;) {
        const TransportResult res = _plain.read(record.data(), std::min(record.size(), step.holdBytes - nbHeld));
        if (res.bytesProcessed == 0) {
          break;  // not received yet: the test fails on the missing message
        }
        _held.append(record.data(), res.bytesProcessed);
        nbHeld += res.bytesProcessed;
      }
      return {0, step.hint};
    }
    if (!_held.empty()) {
      const std::size_t nbBytes = std::min(len, _held.size());
      _held.copy(buf, nbBytes);
      _held.erase(0, nbBytes);
      return {nbBytes, TransportHint::None};
    }
    return _plain.read(buf, len);
  }

  TransportResult write(std::string_view data) { return _plain.write(data); }

  TransportResult write(std::span<const std::string_view> buffers) { return _plain.write(buffers); }

 private:
  PlainTransport _plain;
  std::vector<Step> _steps;
  std::size_t _nextStep{0};
  std::string _held;
};

// Gives 'cnx' a ScriptedReadTransport on its socket, following 'steps'.
void ScriptReads(WebSocketConnection& cnx, std::vector<ScriptedReadTransport::Step> steps) {
  WebSocketConnectionTest::SetTransport(
      cnx, Transport(std::make_unique<ScriptedReadTransport>(WebSocketConnectionTest::Fd(cnx), std::move(steps))));
}

TEST(WebSocketClientScripted, TlsReadWaitingForWritabilityIsRetried) {
  const std::string first = ServerFrame(websocket::Opcode::Text, "first");
  const std::string second = ServerFrame(websocket::Opcode::Text, "second");
  auto sendFirst = std::make_shared<Hold>();
  auto sendSecond = std::make_shared<Hold>();
  auto done = std::make_shared<Hold>();
  test::RawServer server([sendFirst, sendSecond, done, first, second](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd);
    sendFirst->wait();
    test::sendAll(fd, first);
    sendSecond->wait();
    test::sendAll(fd, second);
    done->wait();
  });
  const ReleaseAtExit releaseFirst(sendFirst);
  const ReleaseAtExit releaseSecond(sendSecond);
  const ReleaseAtExit releaseDone(done);
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  ScriptReads(cnx, {{TransportHint::WriteReady, first.size()},
                    {TransportHint::ReadReady, 0},
                    {TransportHint::WriteReady, second.size()}});
  sendFirst->release();

  // The first frame is held by TLS, which has to write first: the writability of the socket is watched.
  ASSERT_TRUE(PollUntil(client, [&cnx] { return WebSocketConnectionTest::Interest(cnx) == kReadWriteEvents; }));
  EXPECT_TRUE(events.messages.empty());
  // A send out of the callbacks is still written at once.
  EXPECT_TRUE(cnx.sendText("meanwhile"));
  EXPECT_EQ(cnx.bufferedAmount(), 0U);

  // Writable: the read is retried, TLS now waits for more bytes, and the writability is not watched anymore.
  client.poll(5s);
  EXPECT_EQ(WebSocketConnectionTest::Interest(cnx), kReadEvents);
  const auto before = std::chrono::steady_clock::now();
  EXPECT_EQ(client.poll(30ms), 1U);  // no busy loop on a writable socket
  EXPECT_GE(std::chrono::steady_clock::now() - before, 30ms);

  // The second frame: TLS has to write first again, then the retried read returns the bytes held.
  sendSecond->release();
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 2; }));
  EXPECT_EQ(events.messages, (std::vector<std::string>{"first", "second"}));
  EXPECT_EQ(WebSocketConnectionTest::Interest(cnx), kReadEvents);
  EXPECT_EQ(events.nbClose, 0);
}

// A plain transport whose writes fail, like on a connection reset by the server.
class FailingWritesTransport final : public TransportBackend<FailingWritesTransport> {
 public:
  explicit FailingWritesTransport(NativeHandle fd) : _plain(fd) {}

  TransportResult read(char* buf, std::size_t len) { return _plain.read(buf, len); }

  static TransportResult write(std::string_view /*data*/) { return {0, TransportHint::Error}; }

 private:
  PlainTransport _plain;
};

TEST(WebSocketClientScripted, CloseFrameFailingTheConnectionCannotBeWritten) {
  auto start = std::make_shared<Hold>();
  auto hold = std::make_shared<Hold>();
  test::RawServer server([start, hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd);
    start->wait();
    test::sendAll(fd, MaskedServerFrame());
    hold->wait();
  });
  const ReleaseAtExit releaseStart(start);
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  WebSocketConnectionTest::SetTransport(
      cnx, Transport(std::make_unique<FailingWritesTransport>(WebSocketConnectionTest::Fd(cnx))));
  start->release();
  // The connection ends anyway, without waiting for the server.
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.closeCode, CloseCode::ProtocolError);
}

#ifdef AERONET_LINUX
TEST(WebSocketClientScripted, EventLoopFailureOfATlsReadWaitingForWritability) {
  // Watching the writability fails, or giving it up once the read is done.
  for (const bool failsToWatch : {true, false}) {
    const std::string frame = ServerFrame(websocket::Opcode::Text, "held");
    auto start = std::make_shared<Hold>();
    auto hold = std::make_shared<Hold>();
    test::RawServer server([start, hold, frame](NativeHandle fd, int /*connIndex*/) {
      AnswerUpgrade(fd);
      start->wait();  // received through the scripted transport, not with the 101 response
      test::sendAll(fd, frame);
      hold->wait();
    });
    const ReleaseAtExit releaseStart(start);
    const ReleaseAtExit releaseAtExit(hold);
    WebSocketClient client(PlainConfig());
    Events events;
    WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
    ASSERT_TRUE(result);
    WebSocketConnection& cnx = *result.pConnection;
    ScriptReads(cnx, {{TransportHint::WriteReady, frame.size()}});
    if (failsToWatch) {
      test::FailAllEpollCtlMod(ENOMEM);
    }
    start->release();
    if (!failsToWatch) {
      ASSERT_TRUE(PollUntil(client, [&cnx] { return WebSocketConnectionTest::Interest(cnx) == kReadWriteEvents; }));
      test::FailAllEpollCtlMod(ENOMEM);
    }
    ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; })) << failsToWatch;
    test::ResetEpollHooks();
    EXPECT_EQ(events.closeCode, CloseCode::AbnormalClosure) << failsToWatch;
    EXPECT_EQ(events.closeReason, "connection error") << failsToWatch;
    EXPECT_TRUE(events.messages.empty()) << failsToWatch;
  }
}
#endif

}  // namespace
}  // namespace aeronet
