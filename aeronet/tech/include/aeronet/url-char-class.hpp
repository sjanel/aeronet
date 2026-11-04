#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aeronet::url {

namespace detail {

// Classes of the bytes of a URI (RFC 3986), as bits of a single table: a rule made of several classes is tested with
// the union of their bits, in a single lookup.
inline constexpr unsigned kUnreserved = 1U << 0U;    // ALPHA / DIGIT / "-" / "." / "_" / "~" (§2.3)
inline constexpr unsigned kSubDelim = 1U << 1U;      // "!" / "$" / "&" / "'" / "(" / ")" / "*" / "+" / "," / ";" / "="
inline constexpr unsigned kPathGenDelim = 1U << 2U;  // ":" / "@" (in a pchar) and "/" (between segments) (§3.3)
inline constexpr unsigned kScheme = 1U << 3U;        // ALPHA / DIGIT / "+" / "-" / "." (§3.1)
inline constexpr unsigned kControl = 1U << 4U;       // CTL: %x00-1F / %x7F (RFC 5234 Appendix B.1)

// Returns true if 'ch' is in one of the classes of 'classMask'.
constexpr bool IsOfClass(char ch, unsigned classMask) noexcept {
  struct ClassTable {
    std::uint8_t data[256];
  };
  static constexpr auto kCharClasses = [] {
    constexpr std::string_view kUnreservedSymbols = "-._~";
    constexpr std::string_view kSubDelims = "!$&'()*+,;=";
    constexpr std::string_view kPathGenDelims = ":@/";
    constexpr std::string_view kSchemeSymbols = "+-.";
    ClassTable table;
    for (std::size_t byte = 0; byte < sizeof(table.data); ++byte) {
      const char ch = static_cast<char>(byte);
      const bool isAlnum = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
      table.data[byte] = static_cast<std::uint8_t>(
          (isAlnum || kUnreservedSymbols.contains(ch) ? kUnreserved : 0U) | (kSubDelims.contains(ch) ? kSubDelim : 0U) |
          (kPathGenDelims.contains(ch) ? kPathGenDelim : 0U) | (isAlnum || kSchemeSymbols.contains(ch) ? kScheme : 0U) |
          (byte < 0x20U || byte == 0x7FU ? kControl : 0U));
    }
    return table;
  }();
  return (kCharClasses.data[static_cast<unsigned char>(ch)] & classMask) != 0;
}

}  // namespace detail

// The predicates are captureless lambdas, not functions: an algorithm taking one monomorphizes on its unique closure
// type, so that the per-byte test always inlines into its loop instead of becoming an indirect call through a function
// pointer.
// None of them accepts '%': the caller handles the pct-encoded octets ("%" HEXDIG HEXDIG) of a URI, and an encoder
// taking them as the set of bytes kept verbatim re-encodes a decoded literal '%' as "%25".

// unreserved (RFC 3986 §2.3): ALPHA / DIGIT / "-" / "." / "_" / "~".
inline constexpr auto IsUnreserved = [](char ch) noexcept { return detail::IsOfClass(ch, detail::kUnreserved); };

// The bytes of a reg-name besides its pct-encoded octets (RFC 3986 §3.2.2): unreserved / sub-delims.
inline constexpr auto IsRegNameChar = [](char ch) noexcept {
  return detail::IsOfClass(ch, detail::kUnreserved | detail::kSubDelim);
};

// The bytes of a path besides its pct-encoded octets (RFC 3986 §3.3): pchar / "/", where
// pchar = unreserved / pct-encoded / sub-delims / ":" / "@".
inline constexpr auto IsPathChar = [](char ch) noexcept {
  return detail::IsOfClass(ch, detail::kUnreserved | detail::kSubDelim | detail::kPathGenDelim);
};

// The bytes of a scheme after its first one, an ALPHA (RFC 3986 §3.1): ALPHA / DIGIT / "+" / "-" / ".".
inline constexpr auto IsSchemeChar = [](char ch) noexcept { return detail::IsOfClass(ch, detail::kScheme); };

// Control characters (CTL: %x00-1F / %x7F), never allowed in a URI (RFC 3986 §2). A bare LF in particular could end the
// request line for lenient parsers.
inline constexpr auto IsControlChar = [](char ch) noexcept { return detail::IsOfClass(ch, detail::kControl); };

}  // namespace aeronet::url
