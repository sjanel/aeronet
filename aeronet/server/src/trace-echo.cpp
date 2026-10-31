#include "trace-echo.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet {

namespace {

// The start of the request line of a TRACE request (RFC 9110 §9.1: the method is case-sensitive).
constexpr std::string_view kTraceRequestLineStart = "TRACE ";

// The header fields likely to contain sensitive data, left out of the echo (RFC 9110 §9.3.8): the credentials (§11.6)
// and the cookies (RFC 6265 §5.4).
constexpr std::string_view kExcludedFields[] = {http::Authorization, http::ProxyAuthorization, http::Cookie};

bool IsExcludedField(std::string_view line) {
  const std::string_view name = line.substr(0, line.find(':'));
  return std::ranges::any_of(kExcludedFields,
                             [name](std::string_view field) { return CaseInsensitiveEqual(name, field); });
}

}  // namespace

bool CopyTraceEcho(std::string_view input, std::size_t maxHeaderBytes, RawChars32& echo) {
  echo.clear();
  if (!input.starts_with(kTraceRequestLineStart)) {
    return true;
  }
  // The head ends with an empty line: the first CRLF following another one.
  const std::size_t emptyLinePos = input.substr(0, maxHeaderBytes).find(http::DoubleCRLF);
  if (emptyLinePos == std::string_view::npos) {
    // Not complete yet, unless already longer than the largest head accepted.
    return input.size() >= maxHeaderBytes;
  }
  const std::string_view head = input.substr(0, emptyLinePos + http::DoubleCRLF.size());

  echo.ensureAvailableCapacity(head.size());
  char* pOut = echo.data();
  // The request line, then the header lines not left out, then the empty line, each line with its CRLF. A malformed
  // line, without colon for instance, is copied as is: such a head is rejected by its parsing.
  std::size_t lineEnd = head.find(http::CRLF) + http::CRLF.size();
  pOut = Append(head.substr(0, lineEnd), pOut);
  for (std::size_t lineBeg = lineEnd; lineBeg != head.size(); lineBeg = lineEnd) {
    lineEnd = head.find(http::CRLF, lineBeg) + http::CRLF.size();
    const std::string_view line = head.substr(lineBeg, lineEnd - lineBeg);
    if (!IsExcludedField(line)) {
      pOut = Append(line, pOut);
    }
  }
  echo.setEnd(pOut);
  return true;
}

}  // namespace aeronet
