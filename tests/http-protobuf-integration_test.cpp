// Protocol Buffers bodies (<aeronet/http-protobuf.hpp>) exchanged between the server and the aeronet HttpClient, over
// HTTP/1.1 and over cleartext HTTP/2, with and without compression.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet-test-required.pb.h"
#include "aeronet-test.pb.h"
#include "aeronet/client-protocol.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-protobuf.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

#ifndef AERONET_PROTOBUF_LITE
#include <google/protobuf/util/json_util.h>
#endif

namespace aeronet {
namespace {

using aeronet_proto_test::Credentials;
using aeronet_proto_test::Order;
using aeronet_proto_test::OrderAck;

constexpr bool kJsonSupported = internal::kProtoJsonSupported<Order>;

// A compression codec compiled in this build (Encoding::none if there is none).
constexpr Encoding kCodec = [] {
  if constexpr (zstdEnabled()) {
    return Encoding::zstd;
  }
  if constexpr (brotliEnabled()) {
    return Encoding::br;
  }
  return zlibEnabled() ? Encoding::gzip : Encoding::none;
}();

// Larger than the 32 KiB of the first compression buffer and than the 16 KiB of an HTTP/2 DATA frame.
constexpr std::size_t kLargePayloadSize = 256UL << 10U;

Order MakeOrder(std::size_t payloadSize = 16) {
  Order order;
  order.set_id(42);
  order.set_customer("jdoe@example.com");
  for (int pos = 0; pos < 3; ++pos) {
    auto* item = order.add_items();
    item->set_sku(std::format("SKU-{}", pos));
    item->set_quantity(static_cast<uint32_t>(pos + 1));
  }
  (*order.mutable_tags())["channel"] = "web";
  order.set_payload(test::MakeDigitsPayload(payloadSize));
  return order;
}

// The acknowledgement of a received order.
OrderAck MakeAck(const HttpRequestView& req, const Order& order) {
  OrderAck ack;
  ack.set_order_id(order.id());
  ack.set_nb_items(static_cast<uint32_t>(order.items_size()));
  ack.set_payload_size(order.payload().size());
  // std::string: the setters of protobuf 3.21 do not take a std::string_view.
  ack.set_content_type(std::string(req.headerValueOrEmpty(http::ContentType)));
  ack.set_original_encoding(std::string(req.headerValueOrEmpty(http::OriginalEncodingHeaderName)));
  return ack;
}

HttpResponse AckOrder(const HttpRequestView& req) {
  auto order = req.bodyAsProto<Order>();
  if (!order) {
    return std::move(order.error());
  }
  return req.makeResponse().bodyProto(MakeAck(req, *order));
}

// Parses into a message of the handler: the overload for messages allocated on an Arena or reused.
HttpResponse EchoOrder(const HttpRequestView& req) {
  Order order;
  if (auto parsed = req.bodyAsProto(order); !parsed) {
    return std::move(parsed.error());
  }
  return req.makeResponse().bodyProto(order);
}

HttpResponse CheckCredentials(const HttpRequestView& req) {
  auto credentials = req.bodyAsProto<Credentials>();
  if (!credentials) {
    return std::move(credentials.error());
  }
  return req.makeResponse(credentials->user());
}

class HttpProtobuf : public ::testing::TestWithParam<HttpVersionMode> {
 protected:
  HttpProtobuf() : ts(HttpServerConfig{}.withPollInterval(std::chrono::milliseconds{5})) {
    ts.router().setPath(http::Method::POST, "/orders", AckOrder);
    ts.router().setPath(http::Method::POST, "/echo", EchoOrder);
    ts.router().setPath(http::Method::POST, "/credentials", CheckCredentials);
    ts.router().setPath(http::Method::GET, "/order", [](const HttpRequestView& req) {
      return req.makeResponse().bodyProto(MakeOrder(kLargePayloadSize));
    });
#ifndef AERONET_PROTOBUF_LITE
    ts.router().setPath(http::Method::POST, "/orders-json", [](const HttpRequestView& req) {
      auto order = req.bodyAsProto<Order>();
      if (!order) {
        return std::move(order.error());
      }
      return req.makeResponse().bodyProtoJson(MakeAck(req, *order));
    });
#endif
    // The encoding of the response negotiated from the Accept header.
    ts.router().setPath(http::Method::GET, "/negotiated",
                        [](const HttpRequestView& req) { return req.makeProtoResponse(MakeOrder()); });
    ts.router().setPath(http::Method::GET, "/negotiated-large",
                        [](const HttpRequestView& req) { return req.makeProtoResponse(MakeOrder(kLargePayloadSize)); });
    ts.router().setPath(http::Method::POST, "/negotiated-strict", [](const HttpRequestView& req) {
      auto order = req.bodyAsProto<Order>();
      if (!order) {
        return std::move(order.error());
      }
      return req.makeProtoResponse(http::StatusCodeCreated, MakeAck(req, *order), ProtoNotAcceptablePolicy::Reject);
    });
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    ts.router().setPath(http::Method::POST, "/async-orders", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
      [[maybe_unused]] const std::string_view body = co_await req.bodyAwaitable();
      co_return AckOrder(req);
    });
#endif
  }

  [[nodiscard]] static HttpClientConfig MakeClientConfig() {
    HttpClientConfig cfg;
    cfg.withHttpVersion(GetParam());
    return cfg;
  }

  [[nodiscard]] std::string url(std::string_view target) const {
    return std::format("http://127.0.0.1:{}{}", ts.port(), target);
  }

  // Sends the request built by 'build' to 'target' with a client of configuration 'cfg' and returns the response.
  template <class Build>
  [[nodiscard]] HttpResponse send(HttpClientConfig cfg, http::Method method, std::string_view target, Build&& build) {
    HttpClient client(std::move(cfg));
    auto request = client.makeRequest(method, url(target));
    std::forward<Build>(build)(request);
    auto result = client.request(std::move(request));
    if (!result) {
      throw std::runtime_error(std::format("request to {} failed", target));
    }
    return std::move(result).value();
  }

  template <class Build>
  [[nodiscard]] HttpResponse send(http::Method method, std::string_view target, Build&& build) {
    return send(MakeClientConfig(), method, target, std::forward<Build>(build));
  }

  // Posts 'order' in its binary encoding to 'target' and returns the acknowledgement of the server.
  [[nodiscard]] OrderAck postOrder(std::string_view target, const Order& order) {
    const HttpResponse resp = send(http::Method::POST, target, [&order](auto& req) { req.bodyProto(order); });
    EXPECT_EQ(resp.status(), http::StatusCodeOK) << resp.bodyInMemory();
    auto ack = resp.bodyAsProto<OrderAck>();
    if (!ack) {
      throw std::runtime_error(ack.error().message);
    }
    return std::move(*ack);
  }

  // Gets 'target' with this Accept header (none if empty).
  [[nodiscard]] HttpResponse getWithAccept(std::string_view target, std::string_view accept) {
    return send(http::Method::GET, target, [accept](auto& req) {
      if (!accept.empty()) {
        req.headerAddLine(http::Accept, accept);
      }
    });
  }

  test::TestServer ts;
};

TEST_P(HttpProtobuf, BinaryRoundTrip) {
  const HttpResponse resp =
      send(http::Method::POST, "/orders", [](auto& req) { req.bodyProto(MakeOrder(), "application/x-protobuf"); });
  ASSERT_EQ(resp.status(), http::StatusCodeOK) << resp.bodyInMemory();
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationProtobuf);
  const auto ack = resp.bodyAsProto<OrderAck>();
  ASSERT_TRUE(ack.has_value()) << ack.error().message;
  EXPECT_EQ(ack->order_id(), 42U);
  EXPECT_EQ(ack->nb_items(), 3U);
  EXPECT_EQ(ack->payload_size(), 16U);
  EXPECT_EQ(ack->content_type(), "application/x-protobuf");
  EXPECT_TRUE(ack->original_encoding().empty());
}

// An empty message is an empty body without Content-Type, in both directions.
TEST_P(HttpProtobuf, EmptyMessages) {
  const HttpResponse resp = send(http::Method::POST, "/orders", [](auto& req) { req.bodyProto(Order{}); });
  ASSERT_EQ(resp.status(), http::StatusCodeOK) << resp.bodyInMemory();
  EXPECT_TRUE(resp.bodyInMemory().empty());
  EXPECT_FALSE(resp.hasHeader(http::ContentType));
  const auto ack = resp.bodyAsProto<OrderAck>();
  ASSERT_TRUE(ack.has_value()) << ack.error().message;
  EXPECT_EQ(ack->order_id(), 0U);
}

TEST_P(HttpProtobuf, LargeMessages) {
  const Order order = MakeOrder(kLargePayloadSize);
  const HttpResponse resp = send(http::Method::POST, "/echo", [&order](auto& req) { req.bodyProto(order); });
  ASSERT_EQ(resp.status(), http::StatusCodeOK);
  Order echoed;
  ASSERT_TRUE(resp.bodyAsProto(echoed).has_value());
  EXPECT_EQ(echoed.id(), order.id());
  EXPECT_EQ(echoed.items_size(), order.items_size());
  EXPECT_EQ(echoed.payload(), order.payload());
}

TEST_P(HttpProtobuf, UnsupportedMediaType) {
  const HttpResponse resp =
      send(http::Method::POST, "/orders", [](auto& req) { req.body("hello", http::ContentTypeTextPlain); });
  EXPECT_EQ(resp.status(), http::StatusCodeUnsupportedMediaType);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Accept),
            kJsonSupported ? "application/protobuf, application/protobuf+json" : "application/protobuf");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeTextPlain);
  EXPECT_TRUE(resp.bodyInMemory().starts_with("Unsupported Content-Type 'text/plain' for a protobuf body"))
      << resp.bodyInMemory();
}

TEST_P(HttpProtobuf, MalformedBody) {
  const HttpResponse resp =
      send(http::Method::POST, "/orders", [](auto& req) { req.body("\x0f", http::ContentTypeApplicationProtobuf); });
  EXPECT_EQ(resp.status(), http::StatusCodeBadRequest);
  EXPECT_FALSE(resp.hasHeader(http::Accept));
  EXPECT_EQ(resp.bodyInMemory(), "Invalid protobuf binary encoding of aeronet_proto_test.Order");
}

TEST_P(HttpProtobuf, MissingRequiredFields) {
  Credentials credentials;
  credentials.set_user("jdoe");
  HttpResponse resp = send(http::Method::POST, "/credentials", [&credentials](auto& req) {
    req.body(credentials.SerializePartialAsString(), http::ContentTypeApplicationProtobuf);
  });
  EXPECT_EQ(resp.status(), http::StatusCodeBadRequest);
  EXPECT_TRUE(resp.bodyInMemory().starts_with("Missing required fields of aeronet_proto_test.Credentials"))
      << resp.bodyInMemory();

  credentials.set_token("secret");
  resp = send(http::Method::POST, "/credentials", [&credentials](auto& req) { req.bodyProto(credentials); });
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "jdoe");
}

#ifndef AERONET_PROTOBUF_LITE
TEST_P(HttpProtobuf, ProtoJson) {
  // ProtoJSON request, binary response.
  HttpResponse resp = send(http::Method::POST, "/orders", [](auto& req) { req.bodyProtoJson(MakeOrder()); });
  ASSERT_EQ(resp.status(), http::StatusCodeOK) << resp.bodyInMemory();
  auto parsed = resp.bodyAsProto<OrderAck>();
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->order_id(), 42U);
  EXPECT_EQ(parsed->content_type(), http::ContentTypeApplicationProtobufJson);

  // Binary request, ProtoJSON response: the client parses it according to its Content-Type.
  resp = send(http::Method::POST, "/orders-json", [](auto& req) { req.bodyProto(MakeOrder()); });
  ASSERT_EQ(resp.status(), http::StatusCodeOK) << resp.bodyInMemory();
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationProtobufJson);
  EXPECT_TRUE(resp.bodyInMemory().contains(R"("orderId":"42")")) << resp.bodyInMemory();
  parsed = resp.bodyAsProto<OrderAck>();
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->nb_items(), 3U);

  // Plain JSON from a client that knows nothing of protobuf.
  resp = send(http::Method::POST, "/orders",
              [](auto& req) { req.body(R"({"id":"9","items":[{"sku":"a"}]})", http::ContentTypeApplicationJson); });
  ASSERT_EQ(resp.status(), http::StatusCodeOK) << resp.bodyInMemory();
  parsed = resp.bodyAsProto<OrderAck>();
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->order_id(), 9U);
  EXPECT_EQ(parsed->nb_items(), 1U);

  resp =
      send(http::Method::POST, "/orders", [](auto& req) { req.body(R"({"id":)", http::ContentTypeApplicationJson); });
  EXPECT_EQ(resp.status(), http::StatusCodeBadRequest);
  EXPECT_TRUE(resp.bodyInMemory().starts_with("Invalid ProtoJSON for aeronet_proto_test.Order: "))
      << resp.bodyInMemory();
}
#endif

// The response is compressed like any other body, and the client decodes it before parsing.
TEST_P(HttpProtobuf, CompressedResponse) {
  if constexpr (kCodec == Encoding::none) {
    GTEST_SKIP() << "no compression codec in this build";
  }
  const Order expected = MakeOrder(kLargePayloadSize);

  // Received compressed.
  HttpClientConfig cfg = MakeClientConfig();
  cfg.withDecompression(false).withDefaultAcceptEncoding(GetEncodingStr(kCodec));
  HttpResponse resp = send(std::move(cfg), http::Method::GET, "/order", [](auto&) {});
  ASSERT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(kCodec));
  EXPECT_LT(resp.bodyInMemory().size(), expected.ByteSizeLong());
  const RawChars decoded = test::Decompress(kCodec, resp.bodyInMemory());
  Order order;
  ASSERT_TRUE(ParseProto(decoded, order, resp.headerValueOrEmpty(http::ContentType)).has_value());
  EXPECT_EQ(order.payload(), expected.payload());

  // Decoded by the client.
  resp = send(http::Method::GET, "/order", [](auto&) {});
  ASSERT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_FALSE(resp.hasHeader(http::ContentEncoding));
  order.Clear();
  ASSERT_TRUE(resp.bodyAsProto(order).has_value());
  EXPECT_EQ(order.payload(), expected.payload());
}

// The client compresses the request body, the server decodes it before the handler parses it.
TEST_P(HttpProtobuf, CompressedRequest) {
  if constexpr (kCodec == Encoding::none) {
    GTEST_SKIP() << "no compression codec in this build";
  }
  HttpClientConfig cfg = MakeClientConfig();
  cfg.withRequestCompression(kCodec);
  const Order order = MakeOrder(kLargePayloadSize);
  const HttpResponse resp =
      send(std::move(cfg), http::Method::POST, "/orders", [&order](auto& req) { req.bodyProto(order); });
  ASSERT_EQ(resp.status(), http::StatusCodeOK) << resp.bodyInMemory();
  const auto ack = resp.bodyAsProto<OrderAck>();
  ASSERT_TRUE(ack.has_value()) << ack.error().message;
  EXPECT_EQ(ack->payload_size(), kLargePayloadSize);
  EXPECT_EQ(ack->original_encoding(), GetEncodingStr(kCodec));
  EXPECT_EQ(ack->content_type(), http::ContentTypeApplicationProtobuf);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST_P(HttpProtobuf, AsyncHandler) {
  const OrderAck ack = postOrder("/async-orders", MakeOrder(kLargePayloadSize));
  EXPECT_EQ(ack.order_id(), 42U);
  EXPECT_EQ(ack.payload_size(), kLargePayloadSize);
}
#endif

TEST_P(HttpProtobuf, NegotiatedBinary) {
  for (std::string_view accept : {
           "",
           "application/protobuf",
           "*/*",
           "application/x-protobuf",
           "application/json;q=0.5, application/protobuf",
           "text/html, */*;q=0.1",
       }) {
    const HttpResponse resp = getWithAccept("/negotiated", accept);
    ASSERT_EQ(resp.status(), http::StatusCodeOK) << accept;
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationProtobuf) << accept;
    // Without ProtoJSON, the response is the same whatever the Accept header.
    EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), kJsonSupported ? http::Accept : std::string_view{}) << accept;
    Order order;
    ASSERT_TRUE(resp.bodyAsProto(order).has_value()) << accept;
    EXPECT_EQ(order.payload(), MakeOrder().payload());
  }
}

TEST_P(HttpProtobuf, NegotiatedJson) {
  for (std::string_view accept : {
           "application/json",
           "application/protobuf+json; charset=utf-8",
           "application/protobuf;q=0.5, application/x-protobuf+json;q=0.9",
           "*/*, application/protobuf;q=0",
       }) {
    const HttpResponse resp = getWithAccept("/negotiated", accept);
    ASSERT_EQ(resp.status(), http::StatusCodeOK) << accept;
    // Without ProtoJSON, nothing acceptable: the binary encoding by default.
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType),
              kJsonSupported ? http::ContentTypeApplicationProtobufJson : http::ContentTypeApplicationProtobuf)
        << accept;
    Order order;
    ASSERT_TRUE(resp.bodyAsProto(order).has_value()) << accept;
    EXPECT_EQ(order.id(), 42U);
    EXPECT_EQ(order.items_size(), 3);
  }
}

// Nothing acceptable: the binary encoding by default, 406 with the Reject policy.
TEST_P(HttpProtobuf, NegotiatedNotAcceptable) {
  HttpResponse resp = getWithAccept("/negotiated", "text/html");
  ASSERT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationProtobuf);
  EXPECT_TRUE(resp.bodyAsProto<Order>().has_value());

  for (std::string_view accept : {"text/html", "application/protobuf;q=0", "application/protobuf;version=2"}) {
    resp = send(http::Method::POST, "/negotiated-strict", [accept](auto& req) {
      req.headerAddLine(http::Accept, accept);
      req.bodyProto(MakeOrder());
    });
    EXPECT_EQ(resp.status(), http::StatusCodeNotAcceptable) << accept;
    EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), http::Accept) << accept;
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeTextPlain) << accept;
    EXPECT_EQ(resp.bodyInMemory(),
              kJsonSupported ? "No acceptable media type for the protobuf response, available: application/protobuf, "
                               "application/protobuf+json"
                             : "No acceptable media type for the protobuf response, available: application/protobuf")
        << accept;
  }

  // An acceptable media type: the status of the handler.
  resp = send(http::Method::POST, "/negotiated-strict", [](auto& req) {
    req.headerAddLine(http::Accept, "application/protobuf");
    req.bodyProto(MakeOrder());
  });
  ASSERT_EQ(resp.status(), http::StatusCodeCreated) << resp.bodyInMemory();
  // The response depends on the Accept header, even without ProtoJSON: it may be a 406.
  EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), http::Accept);
  const auto ack = resp.bodyAsProto<OrderAck>();
  ASSERT_TRUE(ack.has_value()) << ack.error().message;
  EXPECT_EQ(ack->order_id(), 42U);
}

// Vary lists both headers when the negotiated response is compressed.
TEST_P(HttpProtobuf, NegotiatedCompressedResponse) {
  if constexpr (kCodec == Encoding::none) {
    GTEST_SKIP() << "no compression codec in this build";
  }
  const Order expected = MakeOrder(kLargePayloadSize);
  for (std::string_view accept : {"application/protobuf", "application/json"}) {
    HttpClientConfig cfg = MakeClientConfig();
    cfg.withDecompression(false).withDefaultAcceptEncoding(GetEncodingStr(kCodec));
    const HttpResponse resp = send(std::move(cfg), http::Method::GET, "/negotiated-large",
                                   [accept](auto& req) { req.headerAddLine(http::Accept, accept); });
    ASSERT_EQ(resp.status(), http::StatusCodeOK) << accept;
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(kCodec)) << accept;
    EXPECT_EQ(resp.headerValueOrEmpty(http::Vary),
              kJsonSupported ? std::string_view{"accept, accept-encoding"} : http::AcceptEncoding)
        << accept;
    const RawChars decoded = test::Decompress(kCodec, resp.bodyInMemory());
    Order order;
    ASSERT_TRUE(ParseProto(decoded, order, resp.headerValueOrEmpty(http::ContentType)).has_value()) << accept;
    EXPECT_EQ(order.payload(), expected.payload());
  }
}

// The response to HEAD announces the size of the encoding, without body.
TEST(HttpProtobufHead, ContentLengthOfTheEncoding) {
  test::TestServer ts(HttpServerConfig{});
  const Order order = MakeOrder(1000);
  // Captured by value: the handler runs on the server thread, and TSAN does not see the request and its response going
  // through the socket as synchronizing it with the destruction of 'order' at the end of the test.
  ts.router().setPath(http::Method::GET, "/order",
                      [order](const HttpRequestView& req) { return req.makeResponse().bodyProto(order); });
  test::RequestOptions opts;
  opts.method = "HEAD";
  opts.target = "/order";
  const auto resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opts));
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  EXPECT_EQ(test::getHeader(resp, "content-length"), std::to_string(order.ByteSizeLong()));
  EXPECT_EQ(test::getHeader(resp, "content-type"), http::ContentTypeApplicationProtobuf);
  EXPECT_TRUE(resp.body.empty());
}

// HEAD gets the headers of the negotiated GET response, Content-Length included.
TEST(HttpProtobufHead, NegotiatedContentLength) {
  test::TestServer ts(HttpServerConfig{});
  const Order order = MakeOrder(1000);
  // Captured by value: see ContentLengthOfTheEncoding.
  ts.router().setPath(http::Method::GET, "/order",
                      [order](const HttpRequestView& req) { return req.makeProtoResponse(order); });
  test::RequestOptions opts;
  opts.method = "HEAD";
  opts.target = "/order";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opts));
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  EXPECT_EQ(test::getHeader(resp, "content-length"), std::to_string(order.ByteSizeLong()));
  EXPECT_EQ(test::getHeader(resp, "content-type"), http::ContentTypeApplicationProtobuf);
  EXPECT_TRUE(resp.body.empty());

#ifndef AERONET_PROTOBUF_LITE
  std::string json;
  ASSERT_TRUE(google::protobuf::util::MessageToJsonString(order, &json).ok());
  opts.headers.emplace_back("Accept", "application/json");
  resp = test::parseResponseOrThrow(test::requestOrThrow(ts.port(), opts));
  EXPECT_EQ(resp.statusCode, http::StatusCodeOK);
  EXPECT_EQ(test::getHeader(resp, "content-length"), std::to_string(json.size()));
  EXPECT_EQ(test::getHeader(resp, "content-type"), http::ContentTypeApplicationProtobufJson);
  EXPECT_EQ(test::getHeader(resp, "vary"), std::string_view(http::Accept));
  EXPECT_TRUE(resp.body.empty());
#endif
}

std::string ProtocolName(const ::testing::TestParamInfo<HttpVersionMode>& info) {
#ifdef AERONET_ENABLE_HTTP2
  return {info.param == HttpVersionMode::Http2 ? "Http2" : "Http1"};
#else
  return {"Http1"};
#endif
}

#ifdef AERONET_ENABLE_HTTP2
INSTANTIATE_TEST_SUITE_P(Protocols, HttpProtobuf, ::testing::Values(HttpVersionMode::Http1_1, HttpVersionMode::Http2),
                         ProtocolName);
#else
INSTANTIATE_TEST_SUITE_P(Protocols, HttpProtobuf, ::testing::Values(HttpVersionMode::Http1_1), ProtocolName);
#endif

}  // namespace
}  // namespace aeronet
