#include "aeronet/http-protobuf.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet-test-required.pb.h"
#include "aeronet-test.pb.h"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-protobuf-common.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"

#ifndef AERONET_PROTOBUF_LITE
#include <google/protobuf/any.pb.h>
#endif

namespace aeronet {

namespace {

using aeronet_proto_test::Credentials;
using aeronet_proto_test::Item;
using aeronet_proto_test::Order;
using aeronet_proto_test::Tree;

constexpr bool kJsonSupported =
#ifdef AERONET_PROTOBUF_LITE
    false;
#else
    true;
#endif

Order MakeOrder() {
  Order order;
  order.set_id(42);
  order.set_customer("jdoe@example.com");
  order.set_status(aeronet_proto_test::ORDER_STATUS_SHIPPED);
  Item* item = order.add_items();
  item->set_sku("SKU-1");
  item->set_quantity(3);
  item->set_unit_price(9.99);
  item = order.add_items();
  item->set_sku("SKU-2");
  item->set_quantity(1);
  (*order.mutable_tags())["channel"] = "web";
  order.set_payload(std::string(100, 'x'));
  return order;
}

void ExpectSameOrder(const Order& actual, const Order& expected) {
  EXPECT_EQ(actual.id(), expected.id());
  EXPECT_EQ(actual.customer(), expected.customer());
  EXPECT_EQ(actual.status(), expected.status());
  ASSERT_EQ(actual.items_size(), expected.items_size());
  for (int pos = 0; pos < actual.items_size(); ++pos) {
    EXPECT_EQ(actual.items(pos).sku(), expected.items(pos).sku());
    EXPECT_EQ(actual.items(pos).quantity(), expected.items(pos).quantity());
    EXPECT_EQ(actual.items(pos).unit_price(), expected.items(pos).unit_price());
  }
  EXPECT_EQ(actual.tags().size(), expected.tags().size());
  for (const auto& [key, value] : expected.tags()) {
    const auto it = actual.tags().find(key);
    ASSERT_NE(it, actual.tags().end()) << key;
    EXPECT_EQ(it->second, value);
  }
  EXPECT_EQ(actual.payload(), expected.payload());
}

// A string of 'size' bytes whose content is never written: the pages of a large allocation are only mapped when
// touched, so that a string larger than kProtoMaxBodySize costs no memory. Returns an empty string if the allocation
// fails.
std::string UntouchedString(std::size_t size) {
  std::string str;
  try {
    str.resize_and_overwrite(size, [](char* /*data*/, std::size_t len) { return len; });
  } catch (const std::bad_alloc&) {
    str.clear();
  }
  return str;
}

// Sanitizers write the shadow memory of each allocated byte: a 2 GiB allocation would really cost memory.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// ProtoFormatOf
// ---------------------------------------------------------------------------------------------------------------------

TEST(ProtoFormatOf, NoContentTypeIsBinary) { EXPECT_EQ(ProtoFormatOf(""), ProtoFormat::Binary); }

TEST(ProtoFormatOf, BinaryMediaTypes) {
  for (std::string_view contentType :
       {"application/protobuf", "application/x-protobuf", "application/x-protobuffer",
        "application/vnd.google.protobuf", "application/proto", "application/octet-stream", "Application/X-Protobuf",
        "APPLICATION/PROTOBUF", " application/protobuf ", "application/protobuf;"}) {
    EXPECT_EQ(ProtoFormatOf(contentType), ProtoFormat::Binary) << contentType;
  }
}

TEST(ProtoFormatOf, JsonMediaTypes) {
  for (std::string_view contentType :
       {std::string_view("application/protobuf+json"), std::string_view("application/x-protobuf+json"),
        std::string_view("Application/JSON"), http::ContentTypeApplicationJson,
        http::ContentTypeApplicationProtobufJson}) {
    EXPECT_EQ(ProtoFormatOf(contentType), ProtoFormat::Json) << contentType;
  }
}

TEST(ProtoFormatOf, OtherMediaTypesAreUnsupported) {
  for (std::string_view contentType :
       {"text/plain", "application/xml", "application/protobufs", "application/protobuf+xml", "application/grpc",
        "multipart/form-data; boundary=x", "; encoding=binary", "application", "*/*"}) {
    EXPECT_EQ(ProtoFormatOf(contentType), ProtoFormat::Unsupported) << contentType;
  }
}

// RFC 9996 §4: an 'encoding' other than the one of the media type must be treated as an error.
TEST(ProtoFormatOf, EncodingParameter) {
  EXPECT_EQ(ProtoFormatOf("application/protobuf; encoding=binary"), ProtoFormat::Binary);
  EXPECT_EQ(ProtoFormatOf("application/protobuf;encoding=\"BINARY\""), ProtoFormat::Binary);
  EXPECT_EQ(ProtoFormatOf("application/protobuf ; Encoding = binary "), ProtoFormat::Binary);
  EXPECT_EQ(ProtoFormatOf("application/protobuf; encoding=json"), ProtoFormat::Unsupported);
  EXPECT_EQ(ProtoFormatOf("application/protobuf; encoding=base64"), ProtoFormat::Unsupported);
  EXPECT_EQ(ProtoFormatOf("application/protobuf; encoding="), ProtoFormat::Unsupported);
  EXPECT_EQ(ProtoFormatOf("application/protobuf+json; charset=utf-8; encoding=json"), ProtoFormat::Json);
  EXPECT_EQ(ProtoFormatOf("application/protobuf+json; encoding=binary"), ProtoFormat::Unsupported);
  EXPECT_EQ(ProtoFormatOf("application/octet-stream; encoding=json"), ProtoFormat::Unsupported);
}

// RFC 9996 §5: payloads with an unsupported version number must be rejected.
TEST(ProtoFormatOf, VersionParameter) {
  EXPECT_EQ(ProtoFormatOf("application/protobuf; version=1"), ProtoFormat::Binary);
  EXPECT_EQ(ProtoFormatOf("application/protobuf; version=\"1\""), ProtoFormat::Binary);
  EXPECT_EQ(ProtoFormatOf("application/protobuf+json; version=1; charset=utf-8"), ProtoFormat::Json);
  EXPECT_EQ(ProtoFormatOf("application/protobuf; version=2"), ProtoFormat::Unsupported);
  EXPECT_EQ(ProtoFormatOf("application/protobuf+json; VERSION=1.0"), ProtoFormat::Unsupported);
}

TEST(ProtoFormatOf, CharsetParameter) {
  EXPECT_EQ(ProtoFormatOf("application/json; charset=UTF-8"), ProtoFormat::Json);
  EXPECT_EQ(ProtoFormatOf("application/json; charset=\"utf-8\""), ProtoFormat::Json);
  EXPECT_EQ(ProtoFormatOf("application/json; charset=iso-8859-1"), ProtoFormat::Unsupported);
  // Meaningless for the binary encoding: ignored.
  EXPECT_EQ(ProtoFormatOf("application/protobuf; charset=iso-8859-1"), ProtoFormat::Binary);
}

TEST(ProtoFormatOf, OtherParametersAreIgnored) {
  EXPECT_EQ(ProtoFormatOf("application/protobuf; proto=aeronet_proto_test.Order"), ProtoFormat::Binary);
  EXPECT_EQ(ProtoFormatOf("application/protobuf; novalue; ;; x=y"), ProtoFormat::Binary);
  EXPECT_EQ(ProtoFormatOf("application/json; x=\"a b\"; encoding=json"), ProtoFormat::Json);
}

// ---------------------------------------------------------------------------------------------------------------------
// ProtoParseError
// ---------------------------------------------------------------------------------------------------------------------

TEST(ProtoParseError, Status) {
  const auto statusOf = [](ProtoParseErrc code) { return ProtoParseError{.message = {}, .code = code}.status(); };
  EXPECT_EQ(statusOf(ProtoParseErrc::UnsupportedMediaType), http::StatusCodeUnsupportedMediaType);
  EXPECT_EQ(statusOf(ProtoParseErrc::TooLarge), http::StatusCodePayloadTooLarge);
  EXPECT_EQ(statusOf(ProtoParseErrc::Malformed), http::StatusCodeBadRequest);
  EXPECT_EQ(statusOf(ProtoParseErrc::MissingRequiredFields), http::StatusCodeBadRequest);
}

// ---------------------------------------------------------------------------------------------------------------------
// ParseProto
// ---------------------------------------------------------------------------------------------------------------------

TEST(ParseProto, BinaryRoundTrip) {
  const Order order = MakeOrder();
  const std::string encoded = order.SerializeAsString();
  for (std::string_view contentType : {"", "application/protobuf", "application/x-protobuf; version=1"}) {
    Order parsed;
    ASSERT_TRUE(ParseProto(encoded, parsed, contentType).has_value()) << contentType;
    ExpectSameOrder(parsed, order);
  }
}

TEST(ParseProto, EmptyBodyIsTheDefaultMessage) {
  Order parsed = MakeOrder();
  ASSERT_TRUE(ParseProto("", parsed).has_value());
  EXPECT_EQ(parsed.id(), 0U);
  EXPECT_TRUE(parsed.customer().empty());
  EXPECT_EQ(parsed.items_size(), 0);
  EXPECT_TRUE(parsed.tags().empty());
}

// The message is replaced, not merged into.
TEST(ParseProto, ReplacesTheMessage) {
  Order parsed = MakeOrder();
  Order other;
  other.set_id(7);
  ASSERT_TRUE(ParseProto(other.SerializeAsString(), parsed).has_value());
  EXPECT_EQ(parsed.id(), 7U);
  EXPECT_TRUE(parsed.customer().empty());
  EXPECT_EQ(parsed.items_size(), 0);
  EXPECT_TRUE(parsed.tags().empty());
  EXPECT_TRUE(parsed.payload().empty());
}

TEST(ParseProto, TruncatedBinaryIsMalformed) {
  const std::string encoded = MakeOrder().SerializeAsString();
  Order parsed;
  const auto result = ParseProto(std::string_view(encoded).substr(0, encoded.size() - 3), parsed);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ProtoParseErrc::Malformed);
  EXPECT_EQ(result.error().status(), http::StatusCodeBadRequest);
  EXPECT_EQ(result.error().message, "Invalid protobuf binary encoding of aeronet_proto_test.Order");
}

TEST(ParseProto, GarbageIsMalformed) {
  Order parsed;
  // A varint of more than 10 bytes, then a field of wire type 7 (invalid).
  for (std::string_view garbage : {std::string_view("\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01"),
                                   std::string_view("\x0f"), std::string_view("plain text")}) {
    const auto result = ParseProto(garbage, parsed, http::ContentTypeApplicationProtobuf);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ProtoParseErrc::Malformed);
  }
}

// The parser stops at 100 nested messages (default recursion limit of protobuf).
TEST(ParseProto, DeepNestingIsMalformed) {
  const auto makeTree = [](int depth) {
    Tree root;
    Tree* node = &root;
    for (int level = 0; level < depth; ++level) {
      node = node->add_children();
    }
    return root.SerializeAsString();
  };
  Tree parsed;
  EXPECT_TRUE(ParseProto(makeTree(90), parsed).has_value());
  const auto result = ParseProto(makeTree(150), parsed);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ProtoParseErrc::Malformed);
}

TEST(ParseProto, UnsupportedMediaType) {
  Order parsed;
  const auto result = ParseProto(MakeOrder().SerializeAsString(), parsed, "text/plain");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ProtoParseErrc::UnsupportedMediaType);
  EXPECT_EQ(result.error().status(), http::StatusCodeUnsupportedMediaType);
  if constexpr (kJsonSupported) {
    EXPECT_EQ(result.error().message,
              "Unsupported Content-Type 'text/plain' for a protobuf body, expected application/protobuf or "
              "application/protobuf+json");
  } else {
    EXPECT_EQ(result.error().message,
              "Unsupported Content-Type 'text/plain' for a protobuf body, expected application/protobuf");
  }
}

TEST(ParseProto, UnsupportedEncodingParameter) {
  Order parsed;
  const auto result = ParseProto("", parsed, "application/protobuf; encoding=json");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ProtoParseErrc::UnsupportedMediaType);
}

TEST(ParseProto, MissingRequiredFields) {
  Credentials credentials;
  credentials.set_user("jdoe");
  Credentials parsed;
  // SerializePartialAsString: SerializeAsString() asserts that the message is initialized in debug builds.
  const auto result = ParseProto(credentials.SerializePartialAsString(), parsed);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ProtoParseErrc::MissingRequiredFields);
  EXPECT_EQ(result.error().status(), http::StatusCodeBadRequest);
  EXPECT_TRUE(result.error().message.starts_with("Missing required fields of aeronet_proto_test.Credentials: "))
      << result.error().message;
  if constexpr (kJsonSupported) {
    EXPECT_TRUE(result.error().message.ends_with(": token")) << result.error().message;
  }

  credentials.set_token("secret");
  ASSERT_TRUE(ParseProto(credentials.SerializeAsString(), parsed).has_value());
  EXPECT_EQ(parsed.user(), "jdoe");
  EXPECT_EQ(parsed.token(), "secret");
  EXPECT_FALSE(parsed.has_ttl_seconds());
}

TEST(ParseProto, TooLarge) {
  if constexpr (kSanitized) {
    GTEST_SKIP() << "a 2 GiB allocation is not free with sanitizers";
  }
  const std::string body = UntouchedString(kProtoMaxBodySize + 1U);
  if (body.empty()) {
    GTEST_SKIP() << "cannot allocate a body larger than kProtoMaxBodySize";
  }
  Order parsed;
  const auto result = ParseProto(body, parsed);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ProtoParseErrc::TooLarge);
  EXPECT_EQ(result.error().status(), http::StatusCodePayloadTooLarge);
  EXPECT_EQ(result.error().message, "Protobuf body of 2147483648 bytes exceeds the limit of 2147483647 bytes");
}

#ifdef AERONET_PROTOBUF_LITE
TEST(ParseProto, JsonIsUnsupportedWithTheLiteRuntime) {
  Order parsed;
  const auto result = ParseProto(R"({"id":"42"})", parsed, http::ContentTypeApplicationJson);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ProtoParseErrc::UnsupportedMediaType);
}
#else
TEST(ParseProto, Json) {
  Order parsed = MakeOrder();
  for (std::string_view contentType : {http::ContentTypeApplicationJson, http::ContentTypeApplicationProtobufJson,
                                       std::string_view("application/x-protobuf+json")}) {
    ASSERT_TRUE(ParseProto(R"({"id":"7","customer":"bob","status":"ORDER_STATUS_PENDING",)"
                           R"("items":[{"sku":"a","quantity":2,"unitPrice":1.5}],"tags":{"k":"v"}})",
                           parsed, contentType)
                    .has_value())
        << contentType;
    EXPECT_EQ(parsed.id(), 7U);
    EXPECT_EQ(parsed.customer(), "bob");
    EXPECT_EQ(parsed.status(), aeronet_proto_test::ORDER_STATUS_PENDING);
    ASSERT_EQ(parsed.items_size(), 1);
    EXPECT_EQ(parsed.items(0).sku(), "a");
    EXPECT_EQ(parsed.items(0).quantity(), 2U);
    EXPECT_EQ(parsed.items(0).unit_price(), 1.5);
    EXPECT_EQ(parsed.tags().at("k"), "v");
    // Replaced, not merged into: the payload of MakeOrder() is gone.
    EXPECT_TRUE(parsed.payload().empty());
  }
}

TEST(ParseProto, JsonAcceptsProtoFieldNames) {
  Order parsed;
  ASSERT_TRUE(ParseProto(R"({"items":[{"unit_price":2.5}]})", parsed, http::ContentTypeApplicationJson).has_value());
  ASSERT_EQ(parsed.items_size(), 1);
  EXPECT_EQ(parsed.items(0).unit_price(), 2.5);
}

TEST(ParseProto, InvalidJsonIsMalformed) {
  Order parsed;
  for (std::string_view json :
       {std::string_view("not json"), std::string_view(R"({"id":)"), std::string_view(R"({"unknownField":1})"),
        std::string_view(R"({"id":"abc"})"), std::string_view("")}) {
    const auto result = ParseProto(json, parsed, http::ContentTypeApplicationJson);
    ASSERT_FALSE(result.has_value()) << json;
    EXPECT_EQ(result.error().code, ProtoParseErrc::Malformed);
    EXPECT_TRUE(result.error().message.starts_with("Invalid ProtoJSON for aeronet_proto_test.Order: "))
        << result.error().message;
  }
}

TEST(ParseProto, JsonWithAnInvalidEncodingParameterIsUnsupported) {
  Order parsed;
  const auto result = ParseProto("{}", parsed, "application/protobuf+json; encoding=binary");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ProtoParseErrc::UnsupportedMediaType);
}
#endif

// ---------------------------------------------------------------------------------------------------------------------
// HttpResponse::bodyProto / bodyProtoJson / bodyAsProto
// ---------------------------------------------------------------------------------------------------------------------

TEST(HttpResponseProto, BodyProto) {
  Order order = MakeOrder();
  HttpResponse response(http::StatusCodeOK);
  response.bodyProto(order);
  EXPECT_EQ(response.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationProtobuf);
  const std::string_view body = response.bodyInMemory();
  EXPECT_EQ(response.headerValueOrEmpty(http::ContentLength), std::to_string(body.size()));
  EXPECT_EQ(body.size(), order.ByteSizeLong());
  Order parsed;
  ASSERT_TRUE(parsed.ParseFromArray(body.data(), static_cast<int>(body.size())));
  ExpectSameOrder(parsed, order);
}

TEST(HttpResponseProto, BodyProtoWithContentType) {
  const auto response = HttpResponse(http::StatusCodeOK).bodyProto(MakeOrder(), "application/x-protobuf");
  EXPECT_EQ(response.headerValueOrEmpty(http::ContentType), "application/x-protobuf");
  const auto parsed = response.bodyAsProto<Order>();
  ASSERT_TRUE(parsed.has_value());
  ExpectSameOrder(*parsed, MakeOrder());
}

TEST(HttpResponseProto, BodyProtoReplacesTheBody) {
  HttpResponse response(http::StatusCodeOK);
  response.body("previous body, longer than the message", http::ContentTypeTextPlain);
  Order order;
  order.set_id(5);
  response.bodyProto(order);
  EXPECT_EQ(response.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationProtobuf);
  EXPECT_EQ(response.bodyInMemory(), order.SerializeAsString());
}

// An empty message has an empty encoding: the response has no body, which is parsed back as an empty message.
TEST(HttpResponseProto, BodyProtoOfEmptyMessage) {
  HttpResponse response(http::StatusCodeOK);
  response.body("previous body", http::ContentTypeTextPlain);
  response.bodyProto(Order{});
  EXPECT_FALSE(response.hasBody());
  EXPECT_FALSE(response.hasHeader(http::ContentType));
  const auto parsed = response.bodyAsProto<Order>();
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->id(), 0U);
}

TEST(HttpResponseProto, BodyProtoLarge) {
  Order order = MakeOrder();
  order.set_payload(std::string(1U << 20, 'p'));
  HttpResponse response(http::StatusCodeOK);
  response.bodyProto(order);
  Order parsed;
  ASSERT_TRUE(response.bodyAsProto(parsed).has_value());
  ExpectSameOrder(parsed, order);
}

TEST(HttpResponseProto, BodyProtoTooLarge) {
  if constexpr (kSanitized) {
    GTEST_SKIP() << "a 2 GiB allocation is not free with sanitizers";
  }
  std::string payload = UntouchedString(kProtoMaxBodySize);
  if (payload.empty()) {
    GTEST_SKIP() << "cannot allocate a message larger than kProtoMaxBodySize";
  }
  Order order;
  order.set_payload(std::move(payload));
  HttpResponse response(http::StatusCodeOK);
  response.body("kept", http::ContentTypeTextPlain);
  EXPECT_THROW(response.bodyProto(order), std::length_error);
  EXPECT_EQ(response.bodyInMemory(), "kept");
}

TEST(HttpResponseProto, BodyAsProtoErrors) {
  HttpResponse response(http::StatusCodeOK);
  response.body("hello", http::ContentTypeTextPlain);
  auto parsed = response.bodyAsProto<Order>();
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().code, ProtoParseErrc::UnsupportedMediaType);

  response.body("\x0f", http::ContentTypeApplicationProtobuf);
  parsed = response.bodyAsProto<Order>();
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().code, ProtoParseErrc::Malformed);
}

#ifndef AERONET_PROTOBUF_LITE
TEST(HttpResponseProto, BodyProtoJson) {
  const Order order = MakeOrder();
  HttpResponse response(http::StatusCodeOK);
  response.bodyProtoJson(order);
  EXPECT_EQ(response.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationProtobufJson);
  const std::string_view body = response.bodyInMemory();
  EXPECT_TRUE(body.starts_with('{')) << body;
  EXPECT_TRUE(body.contains(R"("customer":"jdoe@example.com")")) << body;
  const auto parsed = response.bodyAsProto<Order>();
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  ExpectSameOrder(*parsed, order);
}

TEST(HttpResponseProto, BodyProtoJsonWithContentType) {
  Order order;
  const auto response = HttpResponse(http::StatusCodeOK).bodyProtoJson(order, http::ContentTypeApplicationJson);
  EXPECT_EQ(response.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationJson);
  EXPECT_EQ(response.bodyInMemory(), "{}");
}

TEST(HttpResponseProto, BodyProtoJsonFailure) {
  google::protobuf::Any any;
  any.set_type_url("type.googleapis.com/aeronet_proto_test.Unknown");
  any.set_value("x");
  HttpResponse response(http::StatusCodeOK);
  response.body("kept", http::ContentTypeTextPlain);
  try {
    response.bodyProtoJson(any);
    FAIL() << "expected std::runtime_error";
  } catch (const std::runtime_error& error) {
    EXPECT_TRUE(std::string_view(error.what()).starts_with("ProtoJSON serialization of google.protobuf.Any failed: "))
        << error.what();
  }
  EXPECT_EQ(response.bodyInMemory(), "kept");
}
#endif

}  // namespace aeronet
