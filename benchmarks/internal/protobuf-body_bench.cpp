// Cost of a protobuf response body, from the message to the connection output buffer (HTTP/1 finalization), for three
// ways of setting it:
//   bodyProto(msg)                      serialized straight into the response buffer (aeronet)
//   body(msg.SerializeAsString(), ...)  serialized into a std::string, captured without copy
//   body(std::string_view(scratch))     serialized into a reused scratch std::string, copied into the response
// And the cost of choosing the encoding of a response from the Accept header (NegotiateProtoFormat, makeProtoResponse).
#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "aeronet-test.pb.h"
#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-protobuf.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"

namespace aeronet {

// HttpResponse befriends this class (also used by the unit tests) to reach the HTTP/1 finalization of the server.
class HttpResponseTest {
 public:
  static HttpMessageData Finalize(HttpResponse& resp, const ConcatenatedHeaders& globalHeaders,
                                  std::size_t minCapturedBodySize) {
    return resp.finalizeForHttp1(kDateHeader, http::HTTP_1_1, HttpResponse::Options{}, &globalHeaders,
                                 minCapturedBodySize);
  }

 private:
  static constexpr const char* kDateHeader = "Thu, 01 Jan 1970 00:00:00 GMT";
};

namespace {

enum class BodyKind : std::uint8_t { BodyProto, SerializeAsString, ScratchCopy };

// An order of 'nbItems' items: about 40 bytes per item.
aeronet_proto_test::Order MakeOrder(int64_t nbItems) {
  aeronet_proto_test::Order order;
  order.set_id(123456789);
  order.set_customer("jdoe@example.com");
  order.set_status(aeronet_proto_test::ORDER_STATUS_SHIPPED);
  for (int64_t pos = 0; pos < nbItems; ++pos) {
    auto* item = order.add_items();
    item->set_sku("SKU-" + std::to_string(pos * 7919));
    item->set_quantity(static_cast<uint32_t>(1 + (pos % 5)));
    item->set_unit_price(static_cast<double>(pos % 1000) / 10.0);
  }
  return order;
}

void BM_ProtoResponse(benchmark::State& state) {
  const auto kind = static_cast<BodyKind>(state.range(0));
  const aeronet_proto_test::Order order = MakeOrder(state.range(1));
  const HttpServerConfig config;
  HttpMessageData outBuffer;
  std::string scratch;

  for ([[maybe_unused]] auto iter : state) {
    HttpResponse resp(http::StatusCodeOK);
    switch (kind) {
      case BodyKind::BodyProto:
        resp.bodyProto(order);
        break;
      case BodyKind::SerializeAsString:
        resp.body(order.SerializeAsString(), http::ContentTypeApplicationProtobuf);
        break;
      default:
        order.SerializeToString(&scratch);
        resp.body(std::string_view(scratch), http::ContentTypeApplicationProtobuf);
        break;
    }
    outBuffer = HttpResponseTest::Finalize(resp, config.globalHeaders, config.minCapturedBodySize);
    benchmark::DoNotOptimize(outBuffer.firstBuffer().data());
    benchmark::DoNotOptimize(outBuffer.secondBuffer().data());
    outBuffer.clear();
  }
  static constexpr std::string_view kNames[] = {"bodyProto", "SerializeAsString", "scratchCopy"};
  state.SetLabel(std::string(kNames[static_cast<std::size_t>(kind)]));
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(order.ByteSizeLong()));
}

void ProtoArgs(benchmark::Benchmark* bench) {
  // About 60 B, 1 KiB, 40 KiB and 400 KiB. Captured bodies below minCapturedBodySize are copied inline at finalization.
  for (const int64_t nbItems : {1, 25, 1000, 10000}) {
    for (const auto kind : {BodyKind::BodyProto, BodyKind::SerializeAsString, BodyKind::ScratchCopy}) {
      bench->Args({static_cast<int64_t>(kind), nbItems});
    }
  }
}

// Accept values of protobuf clients, curl, a JavaScript fetch() of ProtoJSON and a browser.
constexpr std::string_view kAcceptValues[] = {
    "application/protobuf",
    "*/*",
    "application/json",
    "application/protobuf;q=0.5, application/json",
    "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
};

void BM_NegotiateProtoFormat(benchmark::State& state) {
  const std::string_view accept = kAcceptValues[static_cast<std::size_t>(state.range(0))];
  for ([[maybe_unused]] auto iter : state) {
    std::string_view value = accept;
    benchmark::DoNotOptimize(value);
    benchmark::DoNotOptimize(NegotiateProtoFormat(value, true));
  }
  state.SetLabel(std::string(accept));
  state.SetItemsProcessed(state.iterations());
}

}  // namespace

BENCHMARK(BM_ProtoResponse)->Apply(ProtoArgs);
BENCHMARK(BM_NegotiateProtoFormat)->DenseRange(0, static_cast<int64_t>(std::size(kAcceptValues)) - 1);

}  // namespace aeronet

BENCHMARK_MAIN();
