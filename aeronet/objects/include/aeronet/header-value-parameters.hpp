#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/string-trim.hpp"

namespace aeronet::http {

// The first element of a header value followed by parameters (RFC 9110 §5.6.6), without them and without surrounding
// whitespace: the media type of a Content-Type or of a media range of Accept, the coding of an element of
// Accept-Encoding, the disposition type of Content-Disposition... "text/html" for "text/html; charset=utf-8".
constexpr std::string_view ValueWithoutParameters(std::string_view headerValue) noexcept {
  return TrimOws(headerValue.substr(0, headerValue.find(';')));
}

// A parameter of a header value: its name and value without surrounding whitespace, the value without the quotes of a
// quoted string (not unescaped). Some header fields allow a parameter without '=' (Sec-WebSocket-Extensions): its
// value has no data, unlike an empty value given with '='.
struct HeaderParameter {
  [[nodiscard]] constexpr bool hasValue() const noexcept { return value.data() != nullptr; }

  std::string_view name;
  std::string_view value;
};

// Parses one parameter: the text between two ';' of a header value. Its first '=' separates its name from its value.
constexpr HeaderParameter ParseHeaderParameter(std::string_view parameter) noexcept {
  const std::size_t equalPos = parameter.find('=');
  if (equalPos == std::string_view::npos) {
    return {TrimOws(parameter), std::string_view{}};
  }
  return {TrimOws(parameter.substr(0, equalPos)), StripQuotes(TrimOws(parameter.substr(equalPos + 1)))};
}

// Iterates over the parameters of a header value, *( OWS ";" OWS [ parameter ] ) after its first element: the text
// after each ';' that is not in a quoted string, up to the next one, parsed like ParseHeaderParameter() (its first '='
// not in a quoted string separates its name from its value). An empty parameter (";;") has an empty name and no value.
// Nothing follows a ';' that ends the header value.
// Usage:
//   http::HeaderParametersIterator it(contentType);
//   const std::string_view mediaType = it.firstElement();
//   while (it.hasNext()) {
//     const http::HeaderParameter parameter = it.next();
//     ...
//   }
class HeaderParametersIterator {
 public:
  // Iterates over the parameters of 'headerValue', whose first element ends at its first ';': it cannot be quoted.
  explicit constexpr HeaderParametersIterator(std::string_view headerValue) noexcept {
    const std::size_t semicolonPos = headerValue.find(';');
    if (semicolonPos == std::string_view::npos) {
      _firstElement = TrimOws(headerValue);
    } else {
      _firstElement = TrimOws(headerValue.substr(0, semicolonPos));
      _remaining = headerValue.substr(semicolonPos + 1);
    }
  }

  // The first element of the header value, without its parameters and surrounding whitespace (the
  // ValueWithoutParameters() of the header value).
  [[nodiscard]] constexpr std::string_view firstElement() const noexcept { return _firstElement; }

  [[nodiscard]] constexpr bool hasNext() const noexcept { return !_remaining.empty(); }

  // Returns the next parameter and consumes it. Should only be called if hasNext() is true.
  constexpr HeaderParameter next() noexcept {
    const char* const begin = _remaining.data();
    const char* const end = begin + _remaining.size();
    const char* equal = end;  // the first '=' not in a quoted string
    const char* pos = begin;
    while (pos != end && *pos != ';') {
      if (*pos == '"') {
        pos = EndOfQuotedString(pos + 1, end);
      } else {
        if (*pos == '=' && equal == end) {
          equal = pos;
        }
        ++pos;
      }
    }
    _remaining = pos == end ? std::string_view{} : std::string_view(pos + 1, end);
    if (equal == end) {
      return {TrimOws(std::string_view(begin, pos)), std::string_view{}};
    }
    return {TrimOws(std::string_view(begin, equal)), StripQuotes(TrimOws(std::string_view(equal + 1, pos)))};
  }

 private:
  // The end of a quoted string whose content starts at 'pos': just after its closing quote, or 'end' if there is none.
  static constexpr const char* EndOfQuotedString(const char* pos, const char* end) noexcept {
    while (pos != end) {
      const char ch = *pos++;
      if (ch == '"') {
        return pos;
      }
      if (ch == '\\' && pos != end) {
        ++pos;  // a quoted-pair: the escaped character cannot end the string
      }
    }
    return end;
  }

  std::string_view _firstElement;
  std::string_view _remaining;  // the text after the last ';' consumed
};

}  // namespace aeronet::http
