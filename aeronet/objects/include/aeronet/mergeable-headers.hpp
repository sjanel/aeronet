#pragma once

#include <algorithm>
#include <functional>
#include <iterator>
#include <string_view>

#include "aeronet/http-constants.hpp"

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
//   '\0'-> Disallow merge: a duplicate is an error, answered 400 Bad Request (singleton fields, RFC 9110 §5.3).
//         Keeping one occurrence instead would let a proxy in front that keeps another one disagree with us.
//
// Fallback for unknown headers currently returns ',' (optimistic list assumption)
// unless caller disables it.
constexpr char ReqHeaderValueSeparator(std::string_view headerNameLowerCase,
                                       bool mergeAllowedForUnknownRequestHeaders) {
  struct Entry {
    std::string_view name;
    char sep;
  };
  // Table sorted case-sensitively (ASCII) to allow binary search.
  // Separators:
  //   ',' : list headers (ABNF 1#element)
  //   ';' : Cookie concatenation (caller may later insert space after ';')
  //   ' ' : space join (User-Agent)
  //   '\0': duplicate forbidden (Authorization, Content-Length, Host, Range, ...)
  static constexpr Entry kEntries[]{
      {"accept", ','},
      {"accept-charset", ','},
      {"accept-datetime", ','},
      {http::AcceptEncoding, ','},
      {"accept-language", ','},
      {"authorization", '\0'},
      {http::CacheControl, ','},
      {http::Connection, ','},
      {http::ContentLength, '\0'},
      {"content-md5", '\0'},
      {"content-transfer-encoding", '\0'},
      {http::ContentType, '\0'},
      {"cookie", ';'},
      {"dnt", ','},
      {"expect", ','},
      {"forwarded", ','},
      {"from", '\0'},
      {http::Host, '\0'},
      {"http2-settings", '\0'},
      {"if-match", ','},
      {"if-modified-since", '\0'},
      {"if-none-match", ','},
      {"if-range", '\0'},
      {"if-unmodified-since", '\0'},
      {"max-forwards", '\0'},
      {"origin", ','},
      {"pragma", ','},
      {"prefer", ','},
      {"proxy-authorization", '\0'},
      {http::Range, '\0'},
      {"referer", '\0'},
      {"save-data", ','},
      {"sec-fetch-dest", ','},
      {"sec-fetch-mode", ','},
      {"sec-fetch-site", ','},
      {"sec-fetch-user", ','},
      {"sec-websocket-extensions", ','},
      {"sec-websocket-protocol", ','},
      {http::TE, ','},
      {http::Trailer, ','},
      {http::TransferEncoding, ','},
      {http::Upgrade, ','},
      {"upgrade-insecure-requests", '\0'},
      {http::UserAgent, ' '},
      {http::Vary, ','},
      {"via", ','},
      {"warning", ','},
  };

  // The table must be ordered because we use std::ranges::partition_point
  static_assert(std::ranges::is_sorted(kEntries, std::less<>{}, &Entry::name),
                "mergeable header table must be sorted case-sensitively");

  const auto it = std::ranges::partition_point(
      kEntries, [headerNameLowerCase](const Entry& entry) { return entry.name < headerNameLowerCase; });
  if (it != std::end(kEntries) && it->name == headerNameLowerCase) {
    return it->sep;
  }
  // Fallback: assume list semantics for unknown headers and allow comma merge.
  // Rationale: Many extension / experimental headers follow the common 1#token or 1#element pattern.
  // Risk: A truly singleton semantic custom header would now be merged instead of rejected; callers handling
  // security-sensitive custom fields should special-case them before calling this helper.
  return mergeAllowedForUnknownRequestHeaders ? ',' : '\0';
}

}  // namespace aeronet::http