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
// itself without its byte order mark when it is UTF-8, else 'buffer', where it is decoded (its previous contents
// discarded).
// Throws std::bad_alloc on memory exhaustion.
Utf8Html DecodeHtml(std::string_view html, std::string_view contentType, RawChars& buffer);

// The source text of a classic script fetched with the body 'bytes' and the Content-Type 'contentType', decoded to
// UTF-8 as browsers decode it (HTML "fetch a classic script"): from the encoding of its byte order mark, else of the
// charset parameter of 'contentType', else of 'charset' (the charset attribute of the script), else
// 'documentEncoding', else UTF-8. Invalid sequences become U+FFFD. Returns 'bytes' itself, without its byte order mark,
// when it is valid UTF-8, else the text decoded into 'buffer', whose previous contents are discarded. Throws
// std::bad_alloc on memory exhaustion.
[[nodiscard]] std::string_view DecodeScript(std::string_view bytes, std::string_view contentType,
                                            std::string_view charset, std::string_view documentEncoding,
                                            RawChars& buffer);

// 'bytes' decoded as UTF-8, a byte order mark included (WHATWG Encoding "UTF-8 decode without BOM"): 'bytes' itself
// exactly when it is valid UTF-8, else the text decoded into 'buffer' (its previous contents discarded), invalid
// sequences becoming U+FFFD. Throws std::bad_alloc on memory exhaustion.
[[nodiscard]] std::string_view DecodeUtf8(std::string_view bytes, RawChars& buffer);

}  // namespace aeronet::internal
