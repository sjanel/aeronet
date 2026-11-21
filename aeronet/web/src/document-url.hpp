#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "web-url.hpp"

namespace aeronet::internal {

// The URL of a document, serialized by the URL parser and null-terminated: the file name of its inline scripts in the
// stack traces. Its origin ("https://example.com:8443") is a prefix of it, as the URL of a page has no user
// information (the HTTP client refuses those URLs) and only changes within its origin (fragments, history).
class DocumentUrl {
 public:
  // 'url' is an http or https URL serialized by the URL parser. Throws std::bad_alloc on memory exhaustion.
  explicit DocumentUrl(std::string_view url) { assign(url); }

  [[nodiscard]] std::string_view view() const noexcept { return {_chars.data(), _chars.size() - 1U}; }

  [[nodiscard]] const char* c_str() const noexcept { return _chars.data(); }

  [[nodiscard]] std::string_view origin() const noexcept { return {_chars.data(), _originSize}; }

  // Whether the serialized URL 'url' has the origin of this one (HTML "can have its URL rewritten": same scheme, host
  // and port, without user information).
  [[nodiscard]] bool isSameOrigin(std::string_view url) const noexcept { return IsOfOrigin(url, origin()); }

  // Replaces the URL by 'url' (see the constructor). Throws std::bad_alloc on memory exhaustion.
  void assign(std::string_view url);

 private:
  RawChars32 _chars;  // with the null terminator
  uint32_t _originSize{};
};

}  // namespace aeronet::internal
