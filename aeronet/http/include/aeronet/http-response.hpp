#pragma once

#include <cassert>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "flat-hash-map.hpp"
#include "http-constants.hpp"
#include "http-status-code.hpp"

namespace aeronet {

class HttpResponse {
 public:
  using HeadersMap = flat_hash_map<std::string, std::string, std::hash<std::string_view>, std::equal_to<>>;

  // Creates a response with given status code.
  // Usage: HttpResponse(200).reason("OK").body("hello").contentType("text/plain");
  explicit HttpResponse(http::StatusCode code = 200) noexcept : _statusCode(code) {
    assert(_statusCode >= 100 && _statusCode < 1000);
  }

  HttpResponse& statusCode(http::StatusCode statusCode) {
    assert(statusCode >= 100 && statusCode < 1000);
    _statusCode = statusCode;
    return *this;
  }

  [[nodiscard]] http::StatusCode statusCode() const { return _statusCode; }

  // Fluent setters (in-place) accepting string-like or contiguous char ranges.
  template <class S>
    requires((std::is_convertible_v<S, std::string_view>) ||
             (std::ranges::contiguous_range<S> &&
              std::same_as<std::remove_cvref_t<std::ranges::range_value_t<S>>, char>))
  HttpResponse& reason(S&& src) {
    assignTo(_reason, std::forward<S>(src));
    return *this;
  }

  [[nodiscard]] std::string_view reason() const { return _reason; }

  template <class S>
    requires((std::is_convertible_v<S, std::string_view>) ||
             (std::ranges::contiguous_range<S> &&
              std::same_as<std::remove_cvref_t<std::ranges::range_value_t<S>>, char>))
  HttpResponse& body(S&& src) {
    assignTo(_body, std::forward<S>(src));
    return *this;
  }

  [[nodiscard]] std::string_view body() const { return _body; }

  template <class S>
    requires((std::is_convertible_v<S, std::string_view>) ||
             (std::ranges::contiguous_range<S> &&
              std::same_as<std::remove_cvref_t<std::ranges::range_value_t<S>>, char>))
  HttpResponse& contentType(S&& src) {
    return header(http::ContentType, std::forward<S>(src));
  }
  template <class S>
    requires((std::is_convertible_v<S, std::string_view>) ||
             (std::ranges::contiguous_range<S> &&
              std::same_as<std::remove_cvref_t<std::ranges::range_value_t<S>>, char>))
  HttpResponse& location(S&& src) {
    return header(http::Location, std::forward<S>(src));
  }

  // Use this method to insert a new custom header.
  // Do not insert any reserved header (for which IsReservedHeader is true), doing so is undefined behavior.
  template <class S1, class S2>
    requires(std::is_convertible_v<S1, std::string_view>) &&
            (std::is_convertible_v<S2, std::string_view> ||
             (std::ranges::contiguous_range<S2> &&
              std::same_as<std::remove_cvref_t<std::ranges::range_value_t<S2>>, char>))
  HttpResponse& header(S1&& key, S2&& value) {
    assert(!IsReservedHeader(key));
    auto [it, inserted] = _headers.emplace(std::forward<S1>(key), std::forward<S2>(value));
    using HdrLenT = decltype(_headersKVLength);
    if (inserted) {
      _headersKVLength += static_cast<HdrLenT>(it->first.size()) + static_cast<HdrLenT>(it->second.size());
    } else {
      auto oldHeaderLen = static_cast<HdrLenT>(it->first.size()) + static_cast<HdrLenT>(it->second.size());
      it->second = std::forward<S2>(value);
      _headersKVLength +=
          static_cast<HdrLenT>(it->first.size()) + static_cast<HdrLenT>(it->second.size()) - oldHeaderLen;
    }
    return *this;
  }

  [[nodiscard]] const HeadersMap& headers() const { return _headers; }

  [[nodiscard]] std::size_t headersTotalLen() const {
    return static_cast<std::size_t>(_headersKVLength) +
           ((http::CRLF.size() + http::HeaderSep.size()) * _headers.size());
  }

  // Centralized rule for headers the user may not set directly (normal or streaming path).
  // These are either automatically emitted (Date, Content-Length, Connection, Transfer-Encoding) or
  // would create ambiguous / unsupported semantics if user-supplied before dedicated feature support
  // (Trailer, Upgrade, TE). Keeping this here allows future optimization of storage layout without
  // scattering the logic.
  // You can use 'static_assert' to make sure at compilation time that the header you are about to insert is not
  // reserved. The list of reserved headers is unlikely to change in the future, but they are mostly technical /
  // framework headers that aeronet manages internally and probably not very interesting for the client.
  // Example:
  //     static_assert(!aeronet::HttpResponse::IsReservedHeader("X-My-Header")); // OK
  //     static_assert(!aeronet::HttpResponse::IsReservedHeader("Content-Length")); // Not OK
  [[nodiscard]] static constexpr bool IsReservedHeader(std::string_view name) noexcept {
    using namespace http;
    return name == Connection || name == Date || name == ContentLength || name == TransferEncoding || name == Trailer ||
           name == Upgrade || name == TE;
  }

 private:
  friend class HttpServer;

  template <class S2>
  static void assignTo(std::string& dest, S2&& source) {
    if constexpr (std::is_same_v<std::remove_cvref_t<S2>, std::string> && std::is_rvalue_reference_v<S2&&>) {
      dest = std::forward<S2>(source);
    } else if constexpr (std::is_convertible_v<S2, std::string_view>) {
      dest.assign(std::string_view(source));
    } else {
      dest.assign(std::ranges::data(source), std::ranges::size(source));
    }
  }

  http::StatusCode _statusCode;
  uint32_t _headersKVLength{};
  std::string _reason;
  HeadersMap _headers;
  std::string _body;
};

}  // namespace aeronet