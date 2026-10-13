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
    trailers.shrink_to_fit();
    sv.shrink_to_fit();
    inBuffer.shrink_to_fit();
  }

  RawChars buf;                 // can be used for any kind of temporary buffer
  RawChars decompressedBody;    // shared body buffer for non-async request decompression
  RawChars32 trailers;          // scratch buffer to preserve request trailers during decompression
  vector<std::string_view> sv;  // scratch views for chunked decoding and protocol gather writes
  // Input buffer lent to the reads of the connections served by a protocol handler (WebSocket, HTTP/2) that have no
  // pending input: the memory of the previous read, still in the CPU caches.
  RawChars inBuffer;
};

}  // namespace aeronet::internal
