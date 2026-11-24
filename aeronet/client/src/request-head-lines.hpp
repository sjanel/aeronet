#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet::internal {

// Append 'wire', an HTTP/1.1 request (its head, whose blank line ending it starts at 'headEnd', then its inline body if
// any), to 'out' with header lines added: 'cookies' at the end of its Cookie line (a request has a single one, RFC
// 6265bis §5.8.3) or in a new one, then 'extraLines' (complete header lines, each starting with CRLF), then the "close"
// connection option when 'addClose' is set. Returns the position of the blank line in the appended bytes.
std::size_t SpliceRequestHeadLines(std::string_view wire, std::size_t headEnd, std::string_view cookies,
                                   std::string_view extraLines, bool addClose, RawChars& out);

}  // namespace aeronet::internal
