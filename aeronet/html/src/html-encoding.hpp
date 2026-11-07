#pragma once

#include <lexbor/encoding/base.h>

#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet::internal {

// An HTML document decoded to UTF-8, the input of the lexbor HTML parser.
struct Utf8Html {
  std::string_view html;
  const lxb_encoding_data_t* pEncoding;  // the encoding it was decoded from
};

// Determines the encoding of 'html' (see HtmlDocument::HtmlDocument) and decodes it to UTF-8: the result is 'html'
// itself without its byte order mark when it is UTF-8, else 'buffer', where it is decoded.
// Throws std::bad_alloc on memory exhaustion.
Utf8Html DecodeHtml(std::string_view html, std::string_view contentType, RawChars& buffer);

}  // namespace aeronet::internal
