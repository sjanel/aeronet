#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"

namespace aeronet::test {

// A server of test pages: GET /html?src=<html> answers <html> as a text/html document, so that each test writes its
// page inline without state shared with the server thread. Each test file adds its own routes.
inline TestServer MakePageServer() {
  HttpServerConfig cfg;
  cfg.withPort(0).withPollInterval(std::chrono::milliseconds{20});
  TestServer server(std::move(cfg));
  server.router().setPath(http::Method::GET, "/html", [](const HttpRequestView& req) {
    return req.makeResponse(req.queryParamValue("src").value_or(""), http::ContentTypeTextHtml);
  });
  return server;
}

// 'str' with every byte other than the unreserved characters of RFC 3986 percent-encoded.
inline std::string PercentEncode(std::string_view str) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  for (const char ch : str) {
    const auto byte = static_cast<unsigned char>(ch);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || ch == '-' ||
        ch == '.' || ch == '_' || ch == '~') {
      encoded.push_back(ch);
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[byte >> 4U]);
      encoded.push_back(kHex[byte & 0xFU]);
    }
  }
  return encoded;
}

// The base URL of 'server' on 'host'.
inline std::string ServerUrl(const TestServer& server, std::string_view host = "127.0.0.1") {
  return "http://" + std::string(host) + ':' + std::to_string(server.port());
}

// The URL of the document 'html' served by 'server' (see MakePageServer()).
inline std::string PageUrl(const TestServer& server, std::string_view html, std::string_view host = "127.0.0.1") {
  return ServerUrl(server, host) + "/html?src=" + PercentEncode(html);
}

// The page of 'url', which must load.
inline WebPage Navigate(WebClient& client, const std::string& url, const NavigateOptions& options = {}) {
  auto page = client.navigate(url, options);
  if (!page) {
    ADD_FAILURE() << url << ": " << ErrcToStr(page.error());
    std::abort();  // the callers dereference the page
  }
  return std::move(*page);
}

// The JSON result of 'script' in 'page', which must not throw.
inline std::string Eval(WebPage& page, std::string_view script) {
  auto result = page.evaluate(script);
  if (!result) {
    ADD_FAILURE() << script << ": " << result.error();
    return {};
  }
  return std::move(*result);
}

}  // namespace aeronet::test
