#pragma once

#include <algorithm>
#include <iterator>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet::http {

// From a header name in an HTTP request, provide the nominal policy indicator.
// This policy applies to both HTTP/1.x and HTTP/2 semantics. Note that HTTP/2
// requires header field names to be transmitted in lowercase on the wire; header
// names are still treated case-insensitively by higher-level code. Callers should
// therefore perform ASCII-lowercasing or case-insensitive comparison before
// encoding/decoding when interacting with HTTP/2.
//
// Policy indicators:
//   ','  -> list-style merge (append comma + new non-empty value)
//   ';'  -> Cookie multi-line merge (semicolon join) per RFC 6265 §5.4
//   ' '  -> space join (User-Agent tokens)
//   'O'  -> Override semantic: do NOT concatenate; caller should keep ONLY the last occurrence ("keep last")
//   '\0'-> Disallow merge (treat duplicates as error OR ignore subsequent depending on higher-level policy)
//
// Fallback for unknown headers currently returns ',' (optimistic list assumption)
// unless caller disables it. 'O' is chosen because it's an ASCII letter not used
// as a list separator, so it's an unambiguous sentinel.
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
      {"Accept-Datetime", ','},
      {http::AcceptEncoding, ','},
      {"Accept-Language", ','},
      {"Authorization", 'O'},
      {http::CacheControl, ','},
      {http::Connection, ','},
      {http::ContentLength, '\0'},
      {"Content-MD5", '\0'},
      {"Content-Transfer-Encoding", '\0'},
      {http::ContentType, 'O'},
      {"Cookie", ';'},
      {"DNT", ','},
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
      {"Origin", ','},
      {"Pragma", ','},
      {"Prefer", ','},
      {"Proxy-Authorization", 'O'},
      {http::Range, 'O'},
      {"Referer", 'O'},
      {"Save-Data", ','},
      {"Sec-Fetch-Dest", ','},
      {"Sec-Fetch-Mode", ','},
      {"Sec-Fetch-Site", ','},
      {"Sec-Fetch-User", ','},
      {"Sec-WebSocket-Extensions", ','},
      {"Sec-WebSocket-Protocol", ','},
      {http::TE, ','},
      {http::Trailer, ','},
      {http::TransferEncoding, ','},
      {http::Upgrade, ','},
      {"Upgrade-Insecure-Requests", '\0'},
      {http::UserAgent, ' '},
      {http::Vary, ','},
      {"Via", ','},
      {"Warning", ','},
  };

  // The table must be ordered because we use std::ranges::partition_point
  static_assert(std::ranges::is_sorted(kEntries, CaseInsensitiveLess, &Entry::name),
                "mergeable header table must be sorted case-insensitively");

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