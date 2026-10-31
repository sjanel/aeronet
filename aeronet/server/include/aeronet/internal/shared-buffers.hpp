#pragma once

#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

struct SharedBuffers {
  // To avoid unbounded memory growth
  void shrink_to_fit() {
    buf.shrink_to_fit();
    decompressedBody.shrink_to_fit();
    trailersOrTraceEcho.shrink_to_fit();
    sv.shrink_to_fit();
  }

  RawChars buf;               // can be used for any kind of temporary buffer
  RawChars decompressedBody;  // shared body buffer for non-async request decompression
  // Two mutually exclusive uses: the request trailers preserved during the decompression of a chunked body, and the
  // head of the HTTP/1 TRACE request being served, as received (see CopyTraceEcho()), only echoed without body.
  RawChars32 trailersOrTraceEcho;
  vector<std::string_view> sv;  // scratch views for chunked decoding and protocol gather writes
};

}  // namespace aeronet::internal
