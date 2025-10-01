#include "http-version.hpp"

#include <charconv>
#include <cstddef>  // std::size_t
#include <cstring>
#include <system_error>  // std::errc
#include <utility>

#include "stringconv.hpp"

namespace aeronet::http {

Version::VersionStr Version::str() const {
  VersionStr ret;
  ret.append_range(kPrefix);
  ret.append_range(IntegralToCharVector(major));
  ret.push_back('.');
  ret.append_range(IntegralToCharVector(minor));
  return ret;
}

// Parse a textual HTTP version token (e.g. "HTTP/1.1") into Version.
// Returns true on success; false if format invalid.
bool parseHttpVersion(const char *first, const char *last, Version &out) {
  if (std::cmp_less(last - first, HTTP_1_0.str().size()) ||
      std::memcmp(first, Version::kPrefix.data(), Version::kPrefix.size()) != 0) {  // minimal length "HTTP/x.y"
    return false;
  }
  first += Version::kPrefix.size();

  auto dot = static_cast<const char *>(std::memchr(first, '.', static_cast<std::size_t>(last - first)));
  if (dot == nullptr) {
    return false;
  }

  return std::from_chars(first, dot, out.major).ec == std::errc() &&
         std::from_chars(dot + 1, last, out.minor).ec == std::errc();
}
}  // namespace aeronet::http