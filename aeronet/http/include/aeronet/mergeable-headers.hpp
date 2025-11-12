#pragma once

#include <algorithm>
#include <iterator>
#include <string_view>

#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet::http {

// From a header name in a HTTP request, provide the nominal policy indicator:
//   ','  -> list-style merge (append comma + new non-empty value)
//   ';'  -> Cookie multi-line merge (semicolon join) per RFC 6265 §5.4
//   ' '  -> space join (User-Agent tokens)
//   'O'  -> Override semantic: do NOT concatenate; caller should keep ONLY the last occurrence ("keep last")
//   '\0'-> Disallow merge (treat duplicates as error OR ignore subsequent depending on higher-level policy)
// Fallback for unknown headers currently returns ',' (optimistic list assumption) unless caller disables it.
// 'O' is chosen because it's an ASCII letter not used as a list separator, so it's an unambiguous sentinel.
constexpr char ReqHeaderValueSeparator(std::string_view headerName, bool mergeAllowedForUnknownRequestHeaders) {
  struct Entry {
    std::string_view name;
    char sep;
  };
  // Table sorted case-insensitively (ASCII) to allow binary search with CaseInsensitiveLess.
  // Separators:
  //   ',' : list headers (ABNF 1#element)
  //   ';' : Cookie concatenation (caller may later insert space after ';')
  //   ' ' : space join (User-Agent)
  //   'O' : override / keep-last semantics (Authorization, Host, Range, ...)
  //   '\0': duplicate forbidden
  static constexpr Entry kEntries[] = {
      {"Accept", ','},
      {"Accept-Charset", ','},
      {"Accept-Encoding", ','},
      {"Accept-Language", ','},
      {"Authorization", 'O'},
      {"Cache-Control", ','},
      {"Connection", ','},
      {"Content-Length", '\0'},
      {"Cookie", ';'},
      {"Expect", ','},
      {"Forwarded", ','},
      {"From", 'O'},
      {"Host", '\0'},
      {"If-Match", ','},
      {"If-Modified-Since", 'O'},
      {"If-None-Match", ','},
      {"If-Range", 'O'},
      {"If-Unmodified-Since", 'O'},
      {"Max-Forwards", 'O'},
      {"Pragma", ','},
      {"Proxy-Authorization", 'O'},
      {"Range", 'O'},
      {"Referer", 'O'},
      {"TE", ','},
      {"Trailer", ','},
      {"Transfer-Encoding", ','},
      {"Upgrade", ','},
      {"User-Agent", ' '},
      {"Via", ','},
      {"Warning", ','},
  };

  // Compile-time ordering validation using adjacent_find to detect any non-strictly-increasing pair.
  // Predicate returns true when ordering is violated (i.e. a >= b in case-insensitive ordering), so kSorted is true
  // only if no such adjacent pair exists.
  static constexpr bool kSorted = []() {
    return std::ranges::adjacent_find(
               kEntries, [](const auto& lhs, const auto& rhs) { return !CaseInsensitiveLess(lhs, rhs); },
               &Entry::name) == std::end(kEntries);
  }();

  static_assert(kSorted, "mergeable header table must be sorted case-insensitively");

  const auto it = std::ranges::partition_point(
      kEntries, [headerName](const Entry& entry) { return CaseInsensitiveLess(entry.name, headerName); });
  if (it != std::end(kEntries) && CaseInsensitiveEqual(it->name, headerName)) {
    return it->sep;
  }
  // Fallback: assume list semantics for unknown headers and allow comma merge.
  // Rationale: Many extension / experimental headers follow the common 1#token or 1#element pattern.
  // Risk: A truly singleton semantic custom header would now be merged instead of rejected; callers handling
  // security-sensitive custom fields should special-case them before calling this helper.
  return mergeAllowedForUnknownRequestHeaders ? ',' : '\0';
}

}  // namespace aeronet::http