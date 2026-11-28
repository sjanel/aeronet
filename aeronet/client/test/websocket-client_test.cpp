#include "aeronet/websocket-client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
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

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/chrono-lite.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
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

  // Number of messages kept for receive(), returned or not.
  [[nodiscard]] static std::size_t NbKept(const WebSocketConnection& cnx) {
    return cnx._pReceiveQueue == nullptr ? 0 : cnx._pReceiveQueue->entries.size();
  }

  // Number of connections of 'client', ended ones not destroyed yet included.
  [[nodiscard]] static std::size_t NbConnectionObjects(const WebSocketClient& client) {
    return client._connections.size();
  }
};

namespace {

using namespace std::chrono_literals;
using websocket::CloseCode;

// How much earlier than its timeout a poll() can end on the steady clock: Windows ends a wait on a tick of its
// interrupt time, which lags the steady clock by up to one tick (15.625 ms by default).
#ifdef AERONET_WINDOWS
constexpr std::chrono::microseconds kMaxEarlyWakeup{15625};
#else
constexpr std::chrono::microseconds kMaxEarlyWakeup{0};
#endif

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
  std::vector<bool> reconnectingInOnClose;
  WebSocketConnection* pForwardTo{nullptr};  // the connection to which onMessage forwards the messages, if any
  std::size_t bufferedAmountInOnClose{0};
  std::chrono::steady_clock::time_point lastOpen;
  int nbOpen{0};
  int nbClose{0};
  http::StatusCode openStatus{0};       // of the 101 response in onOpen
  http::StatusCode handshakeStatus{0};  // of the response of a failed handshake in onClose, 0 if none
  HttpClientErrc handshakeError{HttpClientErrc::noError};
  CloseCode closeCode{CloseCode::Normal};
};

Events& EventsOf(WebSocketConnection& cnx) { return *static_cast<Events*>(cnx.userData()); }

// Records the events of a connection in its user data, an Events.
constexpr WebSocketClientCallbacks kRecording{
    .onOpen =
        [](WebSocketConnection& cnx) {
          Events& events = EventsOf(cnx);
          ++events.nbOpen;
          events.lastOpen = std::chrono::steady_clock::now();
          EXPECT_FALSE(cnx.reconnecting());
          EXPECT_EQ(cnx.handshakeError(), HttpClientErrc::noError);
          const HttpResponse* pResponse = cnx.handshakeResponse();
          EXPECT_NE(pResponse, nullptr);
          events.openStatus = pResponse == nullptr ? http::StatusCode{0} : pResponse->status();
        },
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
          events.reconnectingInOnClose.push_back(cnx.reconnecting());
          const HttpResponse* pResponse = cnx.handshakeResponse();
          events.handshakeStatus = pResponse == nullptr ? http::StatusCode{0} : pResponse->status();
          events.handshakeError = cnx.handshakeError();
          // Nothing can be sent anymore, whatever ended the connection.
          EXPECT_FALSE(cnx.isOpen());
          EXPECT_FALSE(cnx.sendText("after the end"));
          EXPECT_FALSE(cnx.sendPing());
          if (!cnx.reconnecting()) {
            EXPECT_FALSE(cnx.close());  // it would cancel the reconnection otherwise
          }
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

// The opening handshake is a GET, sent without content (RFC 9110 §9.3.1): a body is refused, with or without trailers.
TEST_F(WebSocketClientTest, UpgradeRequestWithBodyThrows) {
  WebSocketClient client;  // permessage-deflate offered by default
  for (const bool withTrailers : {false, true}) {
    const auto makeRequest = [&client, withTrailers] {
      HttpRequest req = client.makeUpgradeRequest(url("/echo"));
      req.body("body");
      if (withTrailers) {
        req.trailerAddLine("x-checksum", "1");
      }
      return req;
    };
    EXPECT_THROW(static_cast<void>(client.connect(makeRequest(), WebSocketClientCallbacks{})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(client.connectAsync(makeRequest(), WebSocketClientCallbacks{})),
                 std::invalid_argument);
  }
  EXPECT_EQ(WebSocketConnectionTest::NbConnectionObjects(client), 0U);
}

// The opening handshake is a GET (RFC 6455 §4.1): another method is refused, also without body.
TEST_F(WebSocketClientTest, UpgradeRequestOtherThanGetThrows) {
  WebSocketClient client(PlainConfig());
  for (const http::Method method : {http::Method::POST, http::Method::HEAD, http::Method::PUT}) {
    const auto makeRequest = [&client, method] {
      HttpRequest req = client.makeUpgradeRequest(url("/echo"));
      req.method(method);
      return req;
    };
    EXPECT_THROW(static_cast<void>(client.connect(makeRequest(), WebSocketClientCallbacks{})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(client.connectAsync(makeRequest(), WebSocketClientCallbacks{})),
                 std::invalid_argument);
  }
  EXPECT_EQ(WebSocketConnectionTest::NbConnectionObjects(client), 0U);
}

TEST_F(WebSocketClientTest, ServerStartsTheClosingHandshake) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.pConnection->sendText("close:4000"));
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.closeCode,
            static_cast<CloseCode>(4000));  // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange) private use code
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

// Poll 'client' until a callback throws an Exception, for 10 s at most. Returns whether one did.
template <class Exception = std::runtime_error>
bool PollUntilACallbackThrows(WebSocketClient& client) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      client.poll(10ms);
    } catch (const Exception&) {
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

// connect() and poll() cannot run from a callback of poll() (a reconnection from onClose, say): they throw
// std::logic_error, which ends the connection whose callback called them, like any exception of a callback.
TEST_F(WebSocketClientTest, ConnectAndPollFromACallbackThrow) {
  for (const bool callPoll : {false, true}) {
    WebSocketClient client(PlainConfig());
    Events events;
    WebSocketClientCallbacks callbacks = kRecording;
    if (callPoll) {
      callbacks.onMessage = [](WebSocketConnection& cnx, std::span<const std::byte> /*payload*/, bool /*isBinary*/) {
        static_cast<void>(cnx.client().poll(0ms));
      };
    } else {
      callbacks.onMessage = [](WebSocketConnection& cnx, std::span<const std::byte> /*payload*/, bool /*isBinary*/) {
        static_cast<void>(cnx.client().connect("ws://127.0.0.1:1/", kRecording));
      };
    }
    WebSocketConnectResult result = client.connect(url("/echo"), callbacks, &events);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.pConnection->sendText("hello"));
    EXPECT_TRUE(PollUntilACallbackThrows<std::logic_error>(client)) << "poll: " << callPoll;
    EXPECT_EQ(client.nbConnections(), 0U);
    EXPECT_EQ(events.nbClose, 0);
    // The client is usable again.
    EXPECT_EQ(client.poll(0ms), 0U);
  }
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
  assigned.postText(result.pConnection->id(), "posted after move");
  ASSERT_TRUE(PollUntil(assigned, [&events] { return events.messages.size() == 2; }));
  EXPECT_EQ(events.messages[0], "after move");
  EXPECT_EQ(events.messages[1], "posted after move");
  EXPECT_FALSE(assigned.config().isServerSide);
  EXPECT_EQ(assigned.httpClient().config().connectTimeout, HttpClientConfig{}.connectTimeout);
}

TEST_F(WebSocketClientTest, ClientWithoutConnectionWaitsTheTimeout) {
  WebSocketClient client;
  const auto before = std::chrono::steady_clock::now();
  EXPECT_EQ(client.poll(20ms), 0U);
  EXPECT_GE(std::chrono::steady_clock::now() - before, 20ms - kMaxEarlyWakeup);
}

TEST_F(WebSocketClientTest, InvalidConfigurationThrows) {
  websocket::WebSocketConfig config;
  config.maxMessageSize = 0;
  EXPECT_THROW(static_cast<void>(WebSocketClient(config)), std::invalid_argument);
  WebSocketReconnectConfig reconnect;
  reconnect.minConnectInterval = -1ms;
  EXPECT_THROW(static_cast<void>(WebSocketClient(PlainConfig(), {}, reconnect)), std::invalid_argument);
  reconnect.minConnectInterval = 0ms;
  reconnect.retry.multiplier = 0.5F;
  EXPECT_THROW(static_cast<void>(WebSocketClient(PlainConfig(), {}, reconnect)), std::invalid_argument);
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

TEST_F(WebSocketClientTest, ConnectAsyncNegotiatesCompression) {
  WebSocketClient client;
  Events events;
  WebSocketClientCallbacks callbacks = kRecording;
  callbacks.onOpen = [](WebSocketConnection& cnx) {
    kRecording.onOpen(cnx);
    EXPECT_TRUE(cnx.handshakeResponse()
                    ->headerValueOrEmpty(websocket::SecWebSocketExtensions)
                    .starts_with("permessage-deflate"));
    EXPECT_TRUE(cnx.sendText(std::string(10000, 'z')));
  };
  client.connectAsync(url("/echo"), callbacks, &events);
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; }));
  EXPECT_EQ(events.messages[0], std::string(10000, 'z'));
}
#endif

#ifdef AERONET_ENABLE_OPENSSL
TEST(WebSocketClientTls, EchoesOverTlsWithAlpnOfferingHttp11Only) {
  auto [certPem, keyPem] = test::MakeEphemeralCertKey("localhost");
  HttpServerConfig serverConfig;
  serverConfig.withPort(0).withPollInterval(std::chrono::milliseconds{5});
  serverConfig.tls.enabled = true;
  serverConfig.tls.withCertPem(certPem).withKeyPem(keyPem);
#ifdef AERONET_ENABLE_HTTP2
  // The server prefers h2: the upgrade connection only offers http/1.1. A server without HTTP/2 refuses to offer h2.
  serverConfig.tls.withTlsAlpnProtocols(std::array<std::string_view, 2>{"h2", "http/1.1"});
#endif
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

  // The same with a non-blocking handshake.
  Events asyncEvents;
  WebSocketClientCallbacks callbacks = kRecording;
  callbacks.onOpen = [](WebSocketConnection& cnx) {
    kRecording.onOpen(cnx);
    cnx.sendText("async over tls");
  };
  client.connectAsync("wss://127.0.0.1:" + std::to_string(server.port()) + "/echo", callbacks, &asyncEvents);
  ASSERT_TRUE(PollUntil(client, [&asyncEvents] { return asyncEvents.messages.size() == 1; }));
  EXPECT_EQ(asyncEvents.messages[0], "async over tls");
}
#endif

// --- Scripted servers: the bytes of the server, byte for byte ---

// Send on 'fd' the 101 response to the upgrade request 'head', with 'extraHeaders' (complete lines, before its
// Connection line), followed by 'after'.
void AnswerUpgradeOf(NativeHandle fd, std::string_view head, std::string_view extraHeaders = {},
                     std::string_view after = {}) {
  static constexpr std::string_view kKeyLine = "sec-websocket-key: ";
  const std::string lower = test::toLower(std::string(head));
  const std::size_t keyPos = lower.find(kKeyLine) + kKeyLine.size();
  const B64EncodedSha1 accept = ComputeWebSocketAccept(head.substr(keyPos, 24));
  std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n";
  response += extraHeaders;
  response += "Connection: Upgrade\r\n";
  response += "Sec-WebSocket-Accept: " + std::string(accept.data(), accept.size()) + "\r\n";
  response += "\r\n";
  response += after;
  test::sendAll(fd, response);
}

// The 101 response to the upgrade request read from 'fd', see AnswerUpgradeOf(). Returns the request.
std::string AnswerUpgrade(NativeHandle fd, std::string_view extraHeaders = {}, std::string_view after = {}) {
  std::string head = test::RecvHead(fd);
  AnswerUpgradeOf(fd, head, extraHeaders, after);
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
  masked[1] = static_cast<char>(static_cast<unsigned char>(masked[1]) |
                                0x80U);  // mask bit, then the masking key and the payload
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

#ifdef AERONET_ENABLE_ZLIB
// The permessage-deflate offer of the configuration is written in place in the upgrade request, after the headers of
// the caller and Sec-WebSocket-Version.
TEST(WebSocketClientScripted, UpgradeRequestCarriesTheDeflateOffer) {
  struct Captured {
    std::string head;
    std::atomic<bool> ready{false};
  };
  auto captured = std::make_shared<Captured>();
  auto hold = std::make_shared<Hold>();
  test::RawServer server([captured, hold](NativeHandle fd, int /*connIndex*/) {
    captured->head = test::RecvHead(fd);
    captured->ready.store(true);  // before the 101, which connect() waits for
    AnswerUpgradeOf(fd, captured->head);
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  websocket::WebSocketConfig config;
  config.deflateConfig.serverNoContextTakeover = true;
  config.deflateConfig.clientMaxWindowBits = 10;
  WebSocketClient client(config);
  HttpRequest request = client.makeUpgradeRequest(RawUrl(server));
  request.headerAddLine(websocket::SecWebSocketProtocol, "chat");
  ASSERT_TRUE(client.connect(std::move(request), WebSocketClientCallbacks{}));
  ASSERT_TRUE(captured->ready.load());
  const std::string head = test::toLower(captured->head);
  EXPECT_TRUE(head.contains(
      "\r\nsec-websocket-protocol: chat\r\nsec-websocket-version: 13\r\n"
      "sec-websocket-extensions: permessage-deflate; server_no_context_takeover; client_max_window_bits=10\r\n"))
      << head;
}
#endif

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

// After the Close frame of the server, the client waits for the server to close the TCP connection, for closeTimeout:
// max() waits as long as needed (the deadline does not overflow).
TEST(WebSocketClientScripted, DrainingWithoutCloseTimeoutWaitsForTheServer) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd, {}, ServerFrame(websocket::Opcode::Close, "\x03\xe8"));
    hold->wait();  // then the connection closes
  });
  ReleaseAtExit releaseAtExit(hold);
  websocket::WebSocketConfig config = PlainConfig();
  config.closeTimeout = std::chrono::milliseconds::max();
  WebSocketClient client(config);
  Events events;
  ASSERT_TRUE(client.connect(RawUrl(server), kRecording, &events));
  ASSERT_TRUE(PollUntil(client, [&events] { return events.nbClose == 1; }));
  EXPECT_EQ(events.closeCode, CloseCode::Normal);

  const auto until = std::chrono::steady_clock::now() + 100ms;
  while (std::chrono::steady_clock::now() < until) {
    ASSERT_EQ(client.poll(10ms), 1U);  // draining
  }
  hold->release();
  EXPECT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbClose, 1);
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
      {
          "second extensions line",
          "Sec-WebSocket-Extensions: permessage-deflate\r\nSec-WebSocket-Extensions: x-unknown\r\n",
          {},
          kBadExtension,
          true,
          true,
      },
      {
          "invalid deflate parameters",
          "Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=16\r\n",
          {},
          "Sec-WebSocket-Extensions of the response holds invalid permessage-deflate parameters",
          true,
          true,
      },
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

TEST(WebSocketClientScripted, EventLoopFailuresOfAsyncConnections) {
  auto hold = std::make_shared<Hold>();
  auto gotRequest = std::make_shared<std::atomic<bool>>(false);
  auto answer = std::make_shared<std::atomic<bool>>(false);
  test::RawServer server([hold, gotRequest, answer](NativeHandle fd, int connIndex) {
    if (connIndex == 0) {
      return;  // the client fails to watch its socket
    }
    const std::string head = test::RecvHead(fd);
    gotRequest->store(true);
    while (!answer->load()) {
      std::this_thread::sleep_for(1ms);
    }
    AnswerUpgradeOf(fd, head, {},
                    ServerFrame(websocket::Opcode::Text, "a") + ServerFrame(websocket::Opcode::Text, "b"));
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  {
    // The socket of the handshake cannot be watched.
    WebSocketClient client(PlainConfig());
    Events events;
    client.connectAsync(RawUrl(server), kRecording, &events);
    test::PushEpollCtlAddAction({-1, ENOMEM});
    ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
    test::ResetEpollHooks();
    EXPECT_EQ(events.handshakeError, HttpClientErrc::ioError);
  }
  {
    // The opened connection cannot watch its socket: it is lost at the next poll().
    WebSocketClient client(PlainConfig());
    Events events;
    WebSocketConnection& cnx = client.connectAsync(RawUrl(server), WebSocketClientCallbacks{}, &events);
    ASSERT_TRUE(PollUntil(client, [&gotRequest] { return gotRequest->load(); }));
    client.poll(0ms);
    test::FailAllEpollCtlMod(ENOMEM);
    answer->store(true);
    const WebSocketReceiveResult received = cnx.receive(10s);
    test::ResetEpollHooks();
    EXPECT_EQ(received.status, WebSocketReceiveResult::Status::Closed);
    EXPECT_EQ(received.closeCode, CloseCode::AbnormalClosure);
    EXPECT_EQ(received.closeReason, "connection error");
  }
}

TEST(WebSocketClientScripted, EventLoopFailureWhenReceiveResumesTheReads) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd, {}, ServerFrame(websocket::Opcode::Text, "a") + ServerFrame(websocket::Opcode::Text, "b"));
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig());
  WebSocketConnectResult result = client.connect(RawUrl(server), WebSocketClientCallbacks{});
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  ASSERT_TRUE(PollUntil(client, [&cnx] { return WebSocketConnectionTest::NbKept(cnx) == 2; }));
  EXPECT_EQ(cnx.receive(0ms).text(), "a");
  EXPECT_EQ(cnx.receive(0ms).text(), "b");
  // Its socket is watched again: the registration fails.
  test::PushEpollCtlAddAction({-1, ENOMEM});
  const WebSocketReceiveResult received = cnx.receive(10s);
  test::ResetEpollHooks();
  EXPECT_EQ(received.status, WebSocketReceiveResult::Status::Closed);
  EXPECT_EQ(received.closeReason, "connection error");
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
  ScriptReads(cnx, {
                       {TransportHint::WriteReady, first.size()},
                       {TransportHint::ReadReady, 0},
                       {TransportHint::WriteReady, second.size()},
                   });
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
  EXPECT_GE(std::chrono::steady_clock::now() - before, 30ms - kMaxEarlyWakeup);

  // The second frame: TLS has to write first again, then the retried read returns the bytes held.
  sendSecond->release();
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 2; }));
  EXPECT_EQ(events.messages, (std::vector<std::string>{"first", "second"}));
  EXPECT_EQ(WebSocketConnectionTest::Interest(cnx), kReadEvents);
  EXPECT_EQ(events.nbClose, 0);
}

// A plain transport whose writes first wait for a record of 'recordSize' bytes of the socket, like TLS during a
// renegotiation: a write reads what the socket holds of it, and only writes once it is complete. The bytes of the
// record are returned first by the next reads.
class WriteAfterARecordTransport final : public TransportBackend<WriteAfterARecordTransport> {
 public:
  WriteAfterARecordTransport(NativeHandle fd, std::size_t recordSize) : _plain(fd), _recordSize(recordSize) {}

  TransportResult read(char* buf, std::size_t len) {
    if (!_held.empty()) {
      const std::size_t nbBytes = std::min(len, _held.size());
      _held.copy(buf, nbBytes);
      _held.erase(0, nbBytes);
      return {nbBytes, TransportHint::None};
    }
    return _plain.read(buf, len);
  }

  TransportResult write(std::string_view data) { return write(std::span<const std::string_view>(&data, 1)); }

  TransportResult write(std::span<const std::string_view> buffers) {
    if (_nbRecordBytes < _recordSize) {
      std::array<char, 256> record;
      while (_nbRecordBytes < _recordSize) {
        const TransportResult res = _plain.read(record.data(), std::min(record.size(), _recordSize - _nbRecordBytes));
        if (res.bytesProcessed == 0) {
          return {0, TransportHint::ReadReady};
        }
        _held.append(record.data(), res.bytesProcessed);
        _nbRecordBytes += res.bytesProcessed;
      }
    }
    return _plain.write(buffers);
  }

 private:
  PlainTransport _plain;
  std::size_t _recordSize;
  std::size_t _nbRecordBytes{0};
  std::string _held;
};

TEST(WebSocketClientScripted, TlsWriteWaitingForReadabilityWhileTheReadsArePaused) {
  const std::string record = ServerFrame(websocket::Opcode::Text, "second");
  auto sendRecord = std::make_shared<Hold>();
  auto done = std::make_shared<Hold>();
  test::RawServer server([sendRecord, done, record](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd, {}, ServerFrame(websocket::Opcode::Text, "first"));
    sendRecord->wait();
    test::sendAll(fd, record);
    done->wait();
  });
  const ReleaseAtExit releaseRecord(sendRecord);
  const ReleaseAtExit releaseDone(done);
  WebSocketClient client(PlainConfig());
  WebSocketConnectResult result = client.connect(RawUrl(server), WebSocketClientCallbacks{});
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  WebSocketConnectionTest::SetTransport(
      cnx, Transport(std::make_unique<WriteAfterARecordTransport>(WebSocketConnectionTest::Fd(cnx), record.size())));
  // The first message waits for receive(): the reads are paused.
  ASSERT_TRUE(PollUntil(client, [&cnx] { return WebSocketConnectionTest::NbKept(cnx) == 1; }));
  EXPECT_EQ(WebSocketConnectionTest::Interest(cnx), 0U);

  // TLS has to read before it writes: the readability of the socket is watched, though the reads stay paused.
  EXPECT_TRUE(cnx.sendText("hello"));
  EXPECT_NE(cnx.bufferedAmount(), 0U);
  EXPECT_EQ(WebSocketConnectionTest::Interest(cnx), EventIn);

  // The record comes: poll() writes without waiting for its timeout, and without reading a message.
  sendRecord->release();
  const auto before = std::chrono::steady_clock::now();
  client.poll(10s);
  EXPECT_LT(std::chrono::steady_clock::now() - before, 5s);
  EXPECT_EQ(cnx.bufferedAmount(), 0U);
  EXPECT_EQ(WebSocketConnectionTest::Interest(cnx), 0U);
  EXPECT_EQ(WebSocketConnectionTest::NbKept(cnx), 1U);

  // The message read by the write comes after the kept one.
  EXPECT_EQ(cnx.receive(10s).text(), "first");
  EXPECT_EQ(cnx.receive(10s).text(), "second");
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

// --- Non-blocking handshakes, reconnection, receive(), other threads ---

// Reconnects after 10 ms, then 20 ms..., up to 'maxAttempts' handshakes in a row. No connection of the tests is stable.
WebSocketReconnectConfig FastReconnect(uint32_t maxAttempts = 10) {
  WebSocketReconnectConfig config;
  config.retry.baseDelay = 10ms;
  config.retry.maxDelay = 10s;
  config.retry.maxAttempts = maxAttempts;
  config.retry.jitter = 0.0F;
  return config;
}

// A port on which nothing listens.
uint16_t ClosedPort() {
  const test::RawServer server([](NativeHandle /*fd*/, int /*connIndex*/) {});
  return server.port();
}

TEST_F(WebSocketClientTest, ConnectAsyncOpensInPoll) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketClientCallbacks callbacks = kRecording;
  callbacks.onOpen = [](WebSocketConnection& cnx) {
    kRecording.onOpen(cnx);
    EXPECT_TRUE(cnx.sendText("first"));
  };
  WebSocketConnection& cnx = client.connectAsync(url("/echo"), callbacks, &events);
  const uint64_t id = cnx.id();
  EXPECT_EQ(events.nbOpen, 0);
  EXPECT_FALSE(cnx.isOpen());
  EXPECT_FALSE(cnx.sendText("too early"));
  EXPECT_FALSE(cnx.sendPing());
  EXPECT_FALSE(cnx.reconnecting());
  EXPECT_EQ(cnx.handshakeResponse(), nullptr);
  EXPECT_EQ(client.nbConnections(), 1U);
  EXPECT_EQ(client.findConnection(id), &cnx);
  EXPECT_EQ(client.findConnection(id + 1U), nullptr);
  EXPECT_EQ(client.nextTimeout(), 0ms);  // its handshake starts at once

  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; }));
  EXPECT_EQ(events.nbOpen, 1);
  EXPECT_EQ(events.openStatus, http::StatusCodeSwitchingProtocols);
  EXPECT_EQ(events.messages[0], "first");
  EXPECT_TRUE(cnx.isOpen());
  EXPECT_EQ(cnx.handshakeResponse(), nullptr);  // only in the callbacks
  EXPECT_TRUE(cnx.close());
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.closeCode, CloseCode::Normal);
  EXPECT_EQ(events.reconnectingInOnClose, std::vector<bool>{false});
  EXPECT_EQ(client.findConnection(id), nullptr);
}

TEST_F(WebSocketClientTest, ConnectAsyncFailuresGoToOnClose) {
  struct Case {
    std::string url;
    std::string_view reason;
    HttpClientErrc error;
    http::StatusCode status;
  };
  const Case cases[]{
      {"ws://127.0.0.1:" + std::to_string(ClosedPort()) + "/", "connection failed", HttpClientErrc::connectFailed, 0},
      {
          url("/plain"),
          "the server did not switch to the WebSocket protocol",
          HttpClientErrc::noError,
          http::StatusCodeOK,
      },
  };
  for (const Case& testCase : cases) {
    WebSocketClient client(PlainConfig());
    Events events;
    const uint64_t id = client.connectAsync(testCase.url, kRecording, &events).id();
    ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; })) << testCase.url;
    EXPECT_EQ(events.nbOpen, 0);
    EXPECT_EQ(events.nbClose, 1);
    EXPECT_EQ(events.closeCode, CloseCode::AbnormalClosure);
    EXPECT_EQ(events.closeReason, testCase.reason);
    EXPECT_EQ(events.handshakeError, testCase.error);
    EXPECT_EQ(events.handshakeStatus, testCase.status);
    EXPECT_EQ(events.reconnectingInOnClose, std::vector<bool>{false});
    EXPECT_EQ(client.findConnection(id), nullptr);

    // Without onMessage: receive() reports the failure.
    WebSocketConnection& pull = client.connectAsync(testCase.url, WebSocketClientCallbacks{});
    const WebSocketReceiveResult received = pull.receive(10s);
    EXPECT_EQ(received.status, WebSocketReceiveResult::Status::Closed);
    EXPECT_EQ(received.closeCode, CloseCode::AbnormalClosure);
    EXPECT_EQ(received.closeReason, testCase.reason);
    EXPECT_EQ(client.poll(0ms), 0U);
    EXPECT_EQ(WebSocketConnectionTest::NbConnectionObjects(client), 0U);
  }
}

TEST_F(WebSocketClientTest, ConnectAsyncResolvesHostNamesOnAThread) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketClientCallbacks callbacks = kRecording;
  callbacks.onOpen = [](WebSocketConnection& cnx) {
    kRecording.onOpen(cnx);
    cnx.sendText("resolved");
  };
  client.connectAsync("ws://localhost:" + std::to_string(ts.port()) + "/echo", callbacks, &events);
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; }));
  EXPECT_EQ(events.messages[0], "resolved");
}

TEST_F(WebSocketClientTest, ConnectAsyncFromACallback) {
  // The first connection opens a second one from its onOpen.
  struct Spawner : Events {
    Events spawned;
    std::string url;
  };
  Spawner spawner;
  spawner.url = url("/echo");
  WebSocketClientCallbacks callbacks = kRecording;
  callbacks.onOpen = [](WebSocketConnection& cnx) {
    kRecording.onOpen(cnx);
    auto& spawnerOfCnx = static_cast<Spawner&>(EventsOf(cnx));
    WebSocketConnection& spawned = cnx.client().connectAsync(spawnerOfCnx.url, kRecording, &spawnerOfCnx.spawned);
    EXPECT_FALSE(spawned.isOpen());
  };
  WebSocketClient client(PlainConfig());
  ASSERT_TRUE(client.connect(url("/echo"), callbacks, static_cast<Events*>(&spawner)));
  EXPECT_EQ(client.nbConnections(), 2U);
  ASSERT_TRUE(PollUntil(client, [&spawner] { return spawner.spawned.nbOpen == 1; }));
}

TEST_F(WebSocketClientTest, CallbackStoppingAnotherHandshake) {
  // Two handshakes whose responses arrive together: the first one opened stops the other, whose event is then ignored.
  struct Pair : Events {
    WebSocketConnection* pFirst{nullptr};
    WebSocketConnection* pSecond{nullptr};
  };
  Pair pair;
  WebSocketClientCallbacks callbacks = kRecording;
  callbacks.onOpen = [](WebSocketConnection& cnx) {
    kRecording.onOpen(cnx);
    auto& pairOfCnx = static_cast<Pair&>(EventsOf(cnx));
    WebSocketConnection* pOther = &cnx == pairOfCnx.pFirst ? pairOfCnx.pSecond : pairOfCnx.pFirst;
    EXPECT_TRUE(pOther->close());
  };
  WebSocketClient client(PlainConfig());
  pair.pFirst = &client.connectAsync(url("/echo"), callbacks, static_cast<Events*>(&pair));
  pair.pSecond = &client.connectAsync(url("/echo"), callbacks, static_cast<Events*>(&pair));
  for (int idx = 0; idx < 3 && pair.nbOpen == 0; ++idx) {
    client.poll(0ms);  // connects, then requests
    std::this_thread::sleep_for(50ms);
  }
  ASSERT_TRUE(PollUntil(client, [&pair] { return pair.nbOpen == 1; }));
  EXPECT_EQ(client.poll(0ms), 1U);
  EXPECT_EQ(pair.nbOpen, 1);
  EXPECT_EQ(pair.nbClose, 0);
}

TEST_F(WebSocketClientTest, ThrowingOnOpenOfAConnectAsync) {
  WebSocketClient client(PlainConfig());
  WebSocketClientCallbacks callbacks;
  callbacks.onOpen = [](WebSocketConnection& /*cnx*/) { throw std::runtime_error("open"); };
  client.connectAsync(url("/echo"), callbacks);
  bool thrown = false;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!thrown && std::chrono::steady_clock::now() < deadline) {
    try {
      client.poll(10ms);
    } catch (const std::runtime_error&) {
      thrown = true;
    }
  }
  EXPECT_TRUE(thrown);
  EXPECT_EQ(client.nbConnections(), 0U);
  EXPECT_EQ(WebSocketConnectionTest::NbConnectionObjects(client), 0U);
}

TEST(WebSocketClientScripted, FailedWriteOfTheLastPassOpeningConnections) {
  // The second connection forwards the message received with its 101 to the first one, already served by poll(): its
  // write fails in the last pass of poll(), whose onClose opens enough connections to move those of the client.
  struct Spawner : Events {
    std::string url;  // of the connections opened by onClose
    Events spawned;
  };
  auto hold = std::make_shared<Hold>();
  test::RawServer first([hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd);
    hold->wait();
  });
  test::RawServer second([hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd, {}, ServerFrame(websocket::Opcode::Text, "forwarded"));
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig());
  Spawner spawner;
  spawner.url = "ws://127.0.0.1:" + std::to_string(ClosedPort()) + "/";
  WebSocketClientCallbacks spawning = kRecording;
  spawning.onClose = [](WebSocketConnection& cnx, CloseCode code, std::string_view reason) {
    kRecording.onClose(cnx, code, reason);
    auto& spawnerOfCnx = static_cast<Spawner&>(EventsOf(cnx));
    for (int idx = 0; idx < 3; ++idx) {
      cnx.client().connectAsync(spawnerOfCnx.url, kRecording, &spawnerOfCnx.spawned);
    }
  };
  WebSocketConnectResult target = client.connect(RawUrl(first), spawning, static_cast<Events*>(&spawner));
  ASSERT_TRUE(target);
  WebSocketConnectionTest::SetTransport(
      *target.pConnection,
      Transport(std::make_unique<FailingWritesTransport>(WebSocketConnectionTest::Fd(*target.pConnection))));
  WebSocketClientCallbacks forwarding = kRecording;
  forwarding.onMessage = [](WebSocketConnection& cnx, std::span<const std::byte> payload, bool isBinary) {
    kRecording.onMessage(cnx, payload, isBinary);
    EXPECT_TRUE(EventsOf(cnx).pForwardTo->sendText(AsText(payload)));
  };
  Events forwarder;
  forwarder.pForwardTo = target.pConnection;
  ASSERT_TRUE(client.connect(RawUrl(second), forwarding, &forwarder));

  EXPECT_EQ(client.poll(0ms), 4U);  // the second one and the 3 opened
  EXPECT_EQ(forwarder.messages, std::vector<std::string>{"forwarded"});
  EXPECT_EQ(spawner.nbClose, 1);
  EXPECT_EQ(spawner.closeReason, "connection error");
  ASSERT_TRUE(PollUntil(client, [&spawner] { return spawner.spawned.nbClose == 3; }));
  EXPECT_EQ(client.nbConnections(), 1U);
}

TEST_F(WebSocketClientTest, ServerCloseFrameIsFollowedByAReconnection) {
  WebSocketClient client(PlainConfig(), {}, FastReconnect());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  EXPECT_TRUE(cnx.sendText("close:1001"));
  ASSERT_TRUE(PollUntil(client, [&events] { return events.nbOpen == 2; }));
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.closeCode, CloseCode::GoingAway);
  EXPECT_EQ(events.reconnectingInOnClose, std::vector<bool>{true});
  EXPECT_EQ(events.handshakeStatus, http::StatusCode{0});  // a session end
  EXPECT_FALSE(cnx.reconnecting());
  EXPECT_TRUE(cnx.sendText("again"));
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; }));
  EXPECT_EQ(events.messages[0], "again");

  // Closed by the client: no reconnection.
  EXPECT_TRUE(cnx.close());
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbOpen, 2);
  EXPECT_EQ(events.reconnectingInOnClose, (std::vector<bool>{true, false}));
}

TEST_F(WebSocketClientTest, MinConnectIntervalSpacesTheAttemptsToAHost) {
  WebSocketReconnectConfig reconnect;
  reconnect.minConnectInterval = 150ms;
  WebSocketClient client(PlainConfig(), {}, reconnect);
  EXPECT_EQ(client.reconnectConfig().minConnectInterval, 150ms);
  Events first;
  Events second;
  Events third;
  const auto start = std::chrono::steady_clock::now();
  client.connectAsync(url("/echo"), kRecording, &first);
  client.connectAsync(url("/echo"), kRecording, &second);
  ASSERT_TRUE(PollUntil(client, [&first] { return first.nbOpen == 1; }));
  // The second one waits for its turn.
  EXPECT_GT(client.nextTimeout(), 0ms);
  EXPECT_LE(client.nextTimeout(), 150ms);
  ASSERT_TRUE(PollUntil(client, [&second] { return second.nbOpen == 1; }));
  EXPECT_GE(second.lastOpen - start, 150ms);
  // connect() waits too, after the second.
  ASSERT_TRUE(client.connect(url("/echo"), kRecording, &third));
  EXPECT_GE(third.lastOpen - start, 300ms);
  EXPECT_EQ(client.nbConnections(), 3U);
}

TEST_F(WebSocketClientTest, MinConnectIntervalSpacesTheAttemptsDueTogether) {
  WebSocketReconnectConfig reconnect;
  reconnect.minConnectInterval = 100ms;
  WebSocketClient client(PlainConfig(), {}, reconnect);
  Events first;
  Events second;
  client.connectAsync(url("/echo"), kRecording, &first);
  client.connectAsync(url("/echo"), kRecording, &second);
  // Both attempts are due long before the next poll() (a busy caller): they still start one after the other.
  std::this_thread::sleep_for(250ms);
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(PollUntil(client, [&first, &second] { return first.nbOpen == 1 && second.nbOpen == 1; }));
  EXPECT_GE(std::max(first.lastOpen, second.lastOpen) - start, 100ms);
}

TEST(WebSocketClientScripted, MinConnectIntervalIgnoresAPlannedReconnection) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int connIndex) {
    AnswerUpgrade(fd);
    if (connIndex != 0) {
      hold->wait();  // the first connection is lost at once
    }
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketReconnectConfig reconnect = FastReconnect();
  reconnect.retry.baseDelay = 10s;
  reconnect.retry.maxDelay = 20s;
  reconnect.minConnectInterval = 100ms;
  WebSocketClient client(PlainConfig(), {}, reconnect);
  Events lost;
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(client.connect(RawUrl(server), kRecording, &lost));
  ASSERT_TRUE(PollUntil(client, [&lost] { return lost.nbClose == 1; }));
  EXPECT_EQ(lost.reconnectingInOnClose, std::vector<bool>{true});  // in 10 s
  // A new connection to the host goes between the first attempt and the reconnection, not after the latter.
  Events fresh;
  ASSERT_TRUE(client.connect(RawUrl(server), kRecording, &fresh));
  EXPECT_GE(fresh.lastOpen - start, 100ms);
  EXPECT_LT(fresh.lastOpen - start, 5s);
  EXPECT_EQ(client.nbConnections(), 2U);
}

TEST_F(WebSocketClientTest, ReceiveReturnsTheMessagesInOrder) {
  WebSocketClient client(PlainConfig());
  WebSocketConnectResult result = client.connect(url("/echo"), WebSocketClientCallbacks{});
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  EXPECT_TRUE(cnx.sendText("hello"));
  EXPECT_TRUE(cnx.sendBinary(AsBytes("bin")));
  EXPECT_TRUE(cnx.sendText("burst:100"));
  WebSocketReceiveResult received = cnx.receive(10s);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.text(), "hello");
  EXPECT_FALSE(received.isBinary);
  received = cnx.receive(10s);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.text(), "bin");
  EXPECT_TRUE(received.isBinary);
  for (int idx = 0; idx < 100; ++idx) {
    received = cnx.receive(10s);
    ASSERT_TRUE(received) << idx;
    EXPECT_EQ(received.text(), "m" + std::to_string(idx));
  }
  received = cnx.receive(20ms);
  EXPECT_FALSE(received);
  EXPECT_EQ(received.status, WebSocketReceiveResult::Status::Timeout);

  EXPECT_TRUE(cnx.close(CloseCode::Normal, "bye"));
  received = cnx.receive(10s);
  EXPECT_EQ(received.status, WebSocketReceiveResult::Status::Closed);
  EXPECT_EQ(received.closeCode, CloseCode::Normal);
  EXPECT_EQ(received.closeReason, "bye");
  // Destroyed by the next poll().
  EXPECT_EQ(client.nbConnections(), 0U);
  EXPECT_EQ(WebSocketConnectionTest::NbConnectionObjects(client), 1U);
  EXPECT_EQ(client.poll(0ms), 0U);
  EXPECT_EQ(WebSocketConnectionTest::NbConnectionObjects(client), 0U);
}

TEST_F(WebSocketClientTest, ReceiveStopsReadingWhileMessagesWait) {
  WebSocketClient client(PlainConfig());
  WebSocketConnectResult result = client.connect(url("/echo"), WebSocketClientCallbacks{});
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  static constexpr int kNbMessages = 3000;
  EXPECT_TRUE(cnx.sendText("burst:" + std::to_string(kNbMessages)));
  ASSERT_TRUE(PollUntil(client, [&cnx] { return WebSocketConnectionTest::NbKept(cnx) != 0; }));
  const std::size_t nbKept = WebSocketConnectionTest::NbKept(cnx);
  EXPECT_EQ(WebSocketConnectionTest::Interest(cnx), 0U);  // its socket is not watched
  for (int idx = 0; idx < 10; ++idx) {
    client.poll(5ms);
  }
  EXPECT_EQ(WebSocketConnectionTest::NbKept(cnx), nbKept);  // nothing more was read
  for (int idx = 0; idx < kNbMessages; ++idx) {
    const WebSocketReceiveResult received = cnx.receive(10s);
    ASSERT_TRUE(received) << idx;
    EXPECT_EQ(received.text(), "m" + std::to_string(idx));
  }
}

TEST_F(WebSocketClientTest, ReceiveMisuseThrows) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult withCallback = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(withCallback);
  EXPECT_THROW(static_cast<void>(withCallback.pConnection->receive(0ms)), std::logic_error);

  // From a callback run by poll().
  WebSocketClientCallbacks callbacks;
  callbacks.onPong = [](WebSocketConnection& cnx, std::span<const std::byte> /*payload*/) {
    ++EventsOf(cnx).nbOpen;
    EXPECT_THROW(static_cast<void>(cnx.receive(0ms)), std::logic_error);
  };
  Events pullEvents;
  WebSocketConnectResult pull = client.connect(url("/echo"), callbacks, &pullEvents);
  ASSERT_TRUE(pull);
  EXPECT_TRUE(pull.pConnection->sendPing());
  ASSERT_TRUE(PollUntil(client, [&pullEvents] { return pullEvents.nbOpen == 1; }));
}

TEST_F(WebSocketClientTest, PostsFromOtherThreads) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  const uint64_t id = result.pConnection->id();
  static constexpr int kNbMessages = 200;
  std::thread poster([&client, id] {
    for (int idx = 0; idx < kNbMessages; ++idx) {
      client.postText(id, "t" + std::to_string(idx));
      client.postBinary(id, AsBytes("b"));
    }
    client.postText(id + 1000U, "to nobody");  // dropped
  });
  ASSERT_TRUE(
      PollUntil(client, [&events] { return events.messages.size() == static_cast<std::size_t>(2 * kNbMessages); }));
  poster.join();
  int nextText = 0;
  for (std::size_t pos = 0; pos < events.messages.size(); ++pos) {
    if (!events.binary[pos]) {
      EXPECT_EQ(events.messages[pos], "t" + std::to_string(nextText++));
    }
  }
  EXPECT_EQ(nextText, kNbMessages);

  std::thread closer([&client, id] { client.postClose(id, CloseCode::GoingAway, "from a thread"); });
  closer.join();
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.closeCode, CloseCode::GoingAway);
  EXPECT_EQ(events.closeReason, "from a thread");
}

TEST(WebSocketClientThreads, WakeupMakesPollReturn) {
  WebSocketClient client;
  std::thread waker([&client] {
    std::this_thread::sleep_for(50ms);
    client.wakeup();
  });
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(client.poll(20s), 0U);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
  waker.join();
  EXPECT_EQ(client.nextTimeout(), std::chrono::milliseconds::max());
}

#ifndef AERONET_WINDOWS
TEST_F(WebSocketClientTest, OuterEventLoopWatchesTheClient) {
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnectResult result = client.connect(url("/echo"), kRecording, &events);
  ASSERT_TRUE(result);
  // The automatic ping of the idle connection, at half of the idle timeout.
  EXPECT_GT(client.nextTimeout(), 1s);
  EXPECT_LE(client.nextTimeout(), 30s);
  EventLoop outer(PollTimeoutPolicy{.baseTimeout = 100ms});
  outer.addOrThrow(EventLoop::EventFd{client.eventLoopHandle(), EventIn});
  EXPECT_TRUE(result.pConnection->sendText("outer"));
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (events.messages.empty() && std::chrono::steady_clock::now() < deadline) {
    if (!outer.poll().empty()) {
      client.poll(0ms);
    }
  }
  ASSERT_EQ(events.messages.size(), 1U);
  EXPECT_EQ(events.messages[0], "outer");
}
#endif

// --- Scripted servers: handshakes and reconnections ---

TEST(WebSocketClientScripted, CloseStopsAHandshakeInProgress) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int /*connIndex*/) {
    test::RecvHead(fd);
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig());
  Events events;
  WebSocketConnection& cnx = client.connectAsync(RawUrl(server), kRecording, &events);
  for (int idx = 0; idx < 5; ++idx) {
    client.poll(5ms);  // the request is sent, not answered
  }
  EXPECT_EQ(client.nbConnections(), 1U);
  EXPECT_GT(client.nextTimeout(), 0ms);  // until the request timeout
  EXPECT_TRUE(cnx.close());
  EXPECT_FALSE(cnx.close());
  EXPECT_EQ(client.nbConnections(), 0U);
  EXPECT_EQ(client.poll(0ms), 0U);
  EXPECT_EQ(events.nbOpen, 0);
  EXPECT_EQ(events.nbClose, 0);
}

TEST(WebSocketClientScripted, AsyncHandshakeTimesOut) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int /*connIndex*/) {
    test::RecvHead(fd);
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  HttpClientConfig httpConfig;
  httpConfig.requestTimeout = 100ms;
  WebSocketClient client(PlainConfig(), httpConfig);
  Events events;
  client.connectAsync(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.handshakeError, HttpClientErrc::timeout);
  EXPECT_EQ(events.closeReason, "operation timed out");
}

TEST(WebSocketClientScripted, ReconnectsAfterTheConnectionIsLost) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int connIndex) {
    if (connIndex == 0) {
      AnswerUpgrade(fd);  // then closed
      return;
    }
    AnswerUpgrade(fd, {}, ServerFrame(websocket::Opcode::Text, "again"));
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig(), {}, FastReconnect());
  EXPECT_TRUE(client.reconnectConfig().enabled());
  Events events;
  WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(result);
  WebSocketConnection* pCnx = result.pConnection;
  ASSERT_TRUE(PollUntil(client, [&events] { return events.messages.size() == 1; }));
  EXPECT_EQ(events.messages[0], "again");
  EXPECT_EQ(events.nbOpen, 2);
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.closeCode, CloseCode::AbnormalClosure);
  EXPECT_EQ(events.reconnectingInOnClose, std::vector<bool>{true});
  EXPECT_EQ(client.findConnection(pCnx->id()), pCnx);
  EXPECT_TRUE(pCnx->isOpen());
  EXPECT_FALSE(pCnx->reconnecting());
}

TEST(WebSocketClientScripted, RefusalsInRetryStatusesAreRetried) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int connIndex) {
    if (connIndex == 0) {
      test::RecvHead(fd);
      test::sendAll(fd, "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 0\r\nContent-Length: 0\r\n\r\n");
      return;
    }
    if (connIndex == 1) {
      test::RecvHead(fd);
      test::sendAll(fd, "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n");
      return;
    }
    AnswerUpgrade(fd);
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig(), {}, FastReconnect());
  Events events;
  client.connectAsync(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(PollUntil(client, [&events] { return events.nbOpen == 1; }));
  EXPECT_EQ(events.nbClose, 2);
  EXPECT_EQ(events.reconnectingInOnClose, (std::vector<bool>{true, true}));
  EXPECT_EQ(events.handshakeStatus, http::StatusCodeTooManyRequests);
}

TEST(WebSocketClientScripted, ReconnectionGivesUpAfterMaxAttempts) {
  WebSocketClient client(PlainConfig(), {}, FastReconnect(3));
  Events events;
  const auto start = std::chrono::steady_clock::now();
  client.connectAsync("ws://127.0.0.1:" + std::to_string(ClosedPort()) + "/", kRecording, &events);
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbClose, 3);
  EXPECT_EQ(events.reconnectingInOnClose, (std::vector<bool>{true, true, false}));
  EXPECT_EQ(events.handshakeError, HttpClientErrc::connectFailed);
  EXPECT_GE(std::chrono::steady_clock::now() - start, 30ms);  // 10 ms, then 20 ms
}

TEST(WebSocketClientScripted, RefusalsNotInRetryStatusesAreNotRetried) {
  for (const std::string_view response : {
           std::string_view{"HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n"},
           // A 101 without Sec-WebSocket-Accept
           std::string_view{"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n"},
       }) {
    test::RawServer server([response](NativeHandle fd, int /*connIndex*/) {
      test::RecvHead(fd);
      test::sendAll(fd, response);
    });
    WebSocketClient client(PlainConfig(), {}, FastReconnect());
    Events events;
    client.connectAsync(RawUrl(server), kRecording, &events);
    ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; })) << response;
    EXPECT_EQ(events.nbClose, 1) << response;
    EXPECT_EQ(events.reconnectingInOnClose, std::vector<bool>{false}) << response;
    EXPECT_NE(events.handshakeStatus, http::StatusCode{0}) << response;
  }
}

TEST(WebSocketClientScripted, CloseInOnCloseCancelsTheReconnection) {
  test::RawServer server([](NativeHandle fd, int /*connIndex*/) { AnswerUpgrade(fd); });
  WebSocketClient client(PlainConfig(), {}, FastReconnect());
  Events events;
  WebSocketClientCallbacks callbacks = kRecording;
  callbacks.onClose = [](WebSocketConnection& cnx, CloseCode code, std::string_view reason) {
    kRecording.onClose(cnx, code, reason);
    EXPECT_TRUE(cnx.reconnecting());
    EXPECT_TRUE(cnx.close());
    EXPECT_FALSE(cnx.reconnecting());
    EXPECT_FALSE(cnx.close());
  };
  ASSERT_TRUE(client.connect(RawUrl(server), callbacks, &events));
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(events.nbOpen, 1);
  EXPECT_EQ(events.nbClose, 1);
}

TEST(WebSocketClientScripted, CloseWhileWaitingToReconnect) {
  test::RawServer server([](NativeHandle fd, int /*connIndex*/) { AnswerUpgrade(fd); });
  WebSocketReconnectConfig reconnect = FastReconnect();
  reconnect.retry.baseDelay = 10s;
  reconnect.retry.maxDelay = 20s;
  WebSocketClient client(PlainConfig(), {}, reconnect);
  Events events;
  WebSocketConnectResult result = client.connect(RawUrl(server), kRecording, &events);
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  ASSERT_TRUE(PollUntil(client, [&events] { return events.nbClose == 1; }));
  EXPECT_EQ(client.poll(0ms), 1U);  // waiting to reconnect
  EXPECT_TRUE(cnx.reconnecting());
  EXPECT_FALSE(cnx.isOpen());
  EXPECT_FALSE(cnx.sendText("while waiting"));
  EXPECT_GT(client.nextTimeout(), 5s);
  client.postText(cnx.id(), "posted while waiting");  // dropped
  EXPECT_EQ(client.poll(0ms), 1U);
  EXPECT_TRUE(cnx.close());
  EXPECT_EQ(client.poll(0ms), 0U);
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(events.nbOpen, 1);
}

TEST(WebSocketClientScripted, StableConnectionsRestartTheAttempts) {
  for (const bool stable : {false, true}) {
    test::RawServer server([stable](NativeHandle fd, int /*connIndex*/) {
      AnswerUpgrade(fd);
      if (stable) {
        std::this_thread::sleep_for(80ms);  // open longer than maxDelay
      }
    });
    WebSocketReconnectConfig reconnect = FastReconnect(2);
    reconnect.retry.baseDelay = 5ms;
    reconnect.retry.maxDelay = 30ms;
    WebSocketClient client(PlainConfig(), {}, reconnect);
    Events events;
    WebSocketClientCallbacks callbacks = kRecording;
    callbacks.onOpen = [](WebSocketConnection& cnx) {
      kRecording.onOpen(cnx);
      if (EventsOf(cnx).nbOpen == 4) {
        // The next end is the last one.
        cnx.close();
      }
    };
    ASSERT_TRUE(client.connect(RawUrl(server), callbacks, &events));
    ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; })) << stable;
    // Short connections count as attempts: 2 handshakes in a row at most. Stable ones start the series again.
    EXPECT_EQ(events.nbOpen, stable ? 4 : 2);
    EXPECT_FALSE(events.reconnectingInOnClose.back());
  }
}

TEST(WebSocketClientScripted, ReceiveReportsTheEndAfterTheLastMessages) {
  test::RawServer server([](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd, {},
                  ServerFrame(websocket::Opcode::Text, "last") + ServerFrame(websocket::Opcode::Close,
                                                                             "\x03\xe9"
                                                                             "bye"));
    test::RecvHead(fd);  // the Close frame of the client, then the end
  });
  WebSocketClient client(PlainConfig());
  WebSocketConnectResult result = client.connect(RawUrl(server), WebSocketClientCallbacks{});
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  WebSocketReceiveResult received = cnx.receive(10s);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.text(), "last");
  received = cnx.receive(10s);
  EXPECT_EQ(received.status, WebSocketReceiveResult::Status::Closed);
  EXPECT_EQ(received.closeCode, CloseCode::GoingAway);
  EXPECT_EQ(received.closeReason, "bye");
  received = cnx.receive(0ms);  // still the end, until the next poll()
  EXPECT_EQ(received.status, WebSocketReceiveResult::Status::Closed);
}

TEST(WebSocketClientScripted, ReceiveWithoutTimeoutReadsTheSocket) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd, {}, ServerFrame(websocket::Opcode::Text, "with the 101"));
    std::this_thread::sleep_for(20ms);
    test::sendAll(fd, ServerFrame(websocket::Opcode::Text, "later"));
    hold->wait();
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig());
  WebSocketConnectResult result = client.connect(RawUrl(server), WebSocketClientCallbacks{});
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  // Each call polls once: the bytes received with the 101, then those of the socket.
  WebSocketReceiveResult received = cnx.receive(0ms);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.text(), "with the 101");
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  do {
    received = cnx.receive(0ms);
  } while (received.status == WebSocketReceiveResult::Status::Timeout && std::chrono::steady_clock::now() < deadline);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.text(), "later");
}

TEST(WebSocketClientScripted, ReceiveAfterTheConnectionEndedWhilePaused) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd, {}, ServerFrame(websocket::Opcode::Text, "kept"));
    hold->wait();  // never answers the Close frame
  });
  const ReleaseAtExit releaseAtExit(hold);
  websocket::WebSocketConfig config = PlainConfig();
  config.closeTimeout = 50ms;
  WebSocketClient client(config);
  WebSocketConnectResult result = client.connect(RawUrl(server), WebSocketClientCallbacks{});
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  ASSERT_TRUE(PollUntil(client, [&cnx] { return WebSocketConnectionTest::NbKept(cnx) == 1; }));
  EXPECT_TRUE(cnx.close());
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
  EXPECT_EQ(WebSocketConnectionTest::NbConnectionObjects(client), 1U);  // kept for receive()
  WebSocketReceiveResult received = cnx.receive(10s);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.text(), "kept");
  received = cnx.receive(10s);
  EXPECT_EQ(received.status, WebSocketReceiveResult::Status::Closed);
  EXPECT_EQ(received.closeCode, CloseCode::AbnormalClosure);
}

TEST(WebSocketClientScripted, ReceiveAcrossAReconnection) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int connIndex) {
    AnswerUpgrade(fd, {}, ServerFrame(websocket::Opcode::Text, connIndex == 0 ? "first" : "second"));
    if (connIndex != 0) {
      hold->wait();
    }
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig(), {}, FastReconnect());
  WebSocketConnectResult result = client.connect(RawUrl(server), WebSocketClientCallbacks{});
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  WebSocketReceiveResult received = cnx.receive(10s);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.text(), "first");
  received = cnx.receive(10s);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.text(), "second");
}

#ifdef AERONET_LINUX
TEST(WebSocketClientScripted, AsyncResolutionFailureAndCancel) {
  {
    // Failed by the getaddrinfo override, without query.
    WebSocketClient client(PlainConfig());
    Events events;
    client.connectAsync("ws://no-such-host.invalid/", kRecording, &events);
    ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
    EXPECT_EQ(events.handshakeError, HttpClientErrc::connectFailed);
  }
  {
    const test::GetAddrInfoHold hold;
    WebSocketClient client(PlainConfig());
    Events events;
    WebSocketConnection& cnx = client.connectAsync("ws://localhost:1/", kRecording, &events);
    EXPECT_EQ(client.poll(0ms), 1U);  // its host is being resolved
    // By the thread of the resolver: the resolution cannot be cancelled anymore.
    ASSERT_TRUE(test::WaitForHeldGetAddrInfoCalls(1));
    EXPECT_TRUE(cnx.close());
    EXPECT_EQ(client.poll(0ms), 0U);
    // The resolution completes later, waking poll() up: ignored.
    test::ReleaseGetAddrInfoCalls();
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(client.poll(10s), 0U);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
    EXPECT_EQ(events.nbClose, 0);
  }
  {
    // The handshake times out while its host is being resolved, a connect failure as for a regular request: the late
    // resolution is ignored.
    const test::GetAddrInfoHold hold;
    HttpClientConfig httpConfig;
    httpConfig.connectTimeout = 50ms;
    WebSocketClient client(PlainConfig(), httpConfig);
    Events events;
    client.connectAsync("ws://localhost:1/", kRecording, &events);
    EXPECT_EQ(client.poll(0ms), 1U);  // its host is being resolved
    // Taken by the thread of the resolver before the timeout, which would otherwise drop it from the queue.
    ASSERT_TRUE(test::WaitForHeldGetAddrInfoCalls(1));
    ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }));
    EXPECT_EQ(events.handshakeError, HttpClientErrc::connectFailed);
    test::ReleaseGetAddrInfoCalls();
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(client.poll(10s), 0U);  // woken up by the resolution
    EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
    EXPECT_EQ(events.nbClose, 1);
  }
}

TEST(WebSocketClientScripted, ThrowingCallbackKeepsTheResolutionsServedWithIt) {
  // Without the fix, the other resolution was dropped: its handshake waited for this timeout.
  HttpClientConfig httpConfig;
  httpConfig.connectTimeout = 60s;
  WebSocketClient client(PlainConfig(), httpConfig);
  Events events;
  WebSocketClientCallbacks callbacks = kRecording;
  callbacks.onClose = [](WebSocketConnection& cnx, CloseCode code, std::string_view reason) {
    kRecording.onClose(cnx, code, reason);
    if (EventsOf(cnx).nbClose == 1) {
      throw std::runtime_error("onClose");
    }
  };
  // Failed by the getaddrinfo override, without query.
  client.connectAsync("ws://first.invalid/", callbacks, &events);
  client.connectAsync("ws://second.invalid/", callbacks, &events);
  EXPECT_EQ(client.poll(0ms), 2U);    // their hosts are being resolved
  std::this_thread::sleep_for(50ms);  // both resolved: served by the same poll()
  ASSERT_TRUE(PollUntilACallbackThrows(client));
  EXPECT_EQ(events.nbClose, 1);
  EXPECT_EQ(client.nbConnections(), 1U);
  ASSERT_TRUE(PollUntil(client, [&client] { return client.nbConnections() == 0; }, 5s));
  EXPECT_EQ(events.nbClose, 2);
  EXPECT_EQ(events.handshakeError, HttpClientErrc::connectFailed);
}

TEST(WebSocketClientScripted, EventLoopFailureWhenAKeptMessagePausesTheReads) {
  auto hold = std::make_shared<Hold>();
  test::RawServer server([hold](NativeHandle fd, int /*connIndex*/) {
    AnswerUpgrade(fd, {}, ServerFrame(websocket::Opcode::Text, "kept"));
    hold->wait();  // reads nothing: the writes of the client stay blocked
  });
  const ReleaseAtExit releaseAtExit(hold);
  WebSocketClient client(PlainConfig());
  WebSocketConnectResult result = client.connect(RawUrl(server), WebSocketClientCallbacks{});
  ASSERT_TRUE(result);
  WebSocketConnection& cnx = *result.pConnection;
  EXPECT_TRUE(cnx.sendBinary(AsBytes(std::string(32UL * 1024UL * 1024UL, 'p'))));
  ASSERT_EQ(WebSocketConnectionTest::Interest(cnx), kReadWriteEvents);
  // The kept message stops the reads: only the writability stays watched, a change that fails.
  test::FailAllEpollCtlMod(ENOMEM);
  WebSocketReceiveResult received = cnx.receive(10s);
  test::ResetEpollHooks();
  ASSERT_TRUE(received);
  EXPECT_EQ(received.text(), "kept");
  received = cnx.receive(10s);
  EXPECT_EQ(received.status, WebSocketReceiveResult::Status::Closed);
  EXPECT_EQ(received.closeReason, "connection error");
}
#endif

}  // namespace
}  // namespace aeronet
