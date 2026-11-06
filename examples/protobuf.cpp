// Protocol Buffers bodies: POST /orders takes an Order, in the binary encoding or as ProtoJSON, and answers a Receipt
// in the encoding that the Accept header prefers (binary for '*/*').
//
//   aeronet-protobuf [port]
//   curl -H 'content-type: application/json' -H 'accept: application/json' \
//        -d '{"customer":"jdoe","items":[{"sku":"BOOK-1","quantity":2,"unitPrice":12.5}]}' http://localhost:8080/orders
//   -> {"orderId":"1","nbItems":2,"total":25}
#include <aeronet/aeronet-server.hpp>
#include <aeronet/http-protobuf.hpp>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <system_error>
#include <utility>

#include "protobuf-shop.pb.h"

using namespace aeronet;

int main(int argc, char** argv) {
  uint16_t port = 8080;
  if (argc > 1) {
    const std::string_view arg1(argv[1]);
    const auto [ptr, errc] = std::from_chars(arg1.data(), arg1.data() + arg1.size(), port);
    if (errc != std::errc{} || ptr != arg1.data() + arg1.size()) {
      std::cerr << "Invalid port number: '" << argv[1] << "'\n";
      return EXIT_FAILURE;
    }
  }

  SignalHandler::Enable();

  uint64_t lastOrderId = 0;  // a SingleHttpServer calls its handlers from a single thread

  Router router;
  router.setPath(http::Method::POST, "/orders", [&lastOrderId](const HttpRequestView& req) {
    // 415 for a Content-Type of no protobuf encoding, 400 for a malformed body.
    auto order = req.bodyAsProto<shop::Order>();
    if (!order) {
      return std::move(order.error());
    }

    shop::Receipt receipt;
    receipt.set_order_id(++lastOrderId);
    double total = 0;
    for (const shop::Item& item : order->items()) {
      receipt.set_nb_items(receipt.nb_items() + item.quantity());
      total += item.unit_price() * item.quantity();
    }
    receipt.set_total(total);

    // Binary or ProtoJSON according to the Accept header, with 'Vary: Accept'. Compressed when the client accepts a
    // content coding and the body is large enough.
    return req.makeProtoResponse(http::StatusCodeCreated, receipt);
  });

  try {
    SingleHttpServer server(HttpServerConfig{}.withPort(port), std::move(router));
    server.run();  // blocking run, until Ctrl+C
  } catch (const std::exception& e) {
    std::cerr << "Server encountered error: " << e.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
