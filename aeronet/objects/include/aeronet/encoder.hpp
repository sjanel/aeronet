#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/encoder-result.hpp"

namespace aeronet {

class EncoderContext {
 public:
  virtual ~EncoderContext() = default;

  // Output room to give encodeChunk() for a chunk of chunkSize bytes: enough in most cases for one call to consume the
  // whole chunk, not a guarantee (see encodeChunk()).
  [[nodiscard]] virtual std::size_t minEncodeChunkCapacity(std::size_t chunkSize) const = 0;

  // Returns the minimal buffer size needed to hold data produced by a single end() call.
  [[nodiscard]] virtual std::size_t endChunkSize() const = 0;

  // Streaming chunk encoder: compresses a prefix of 'data' into 'buf', writing at most 'availableCapacity' bytes, and
  // removes that prefix from 'data'. Returns the number of bytes written, often 0 as encoders buffer their input.
  // 'data' is left non-empty when the output room is full: the caller makes room (emits or grows its buffer) and calls
  // again with the rest. No fixed room avoids it: brotli and zstd emit their output in bursts (a brotli metablock spans
  // several input blocks, up to several MiB) and stop reading input while that output does not fit. A call with at
  // least 1 byte of room always consumes or writes something.
  // You should not call encodeChunk() again after having finished the stream.
  virtual EncoderResult encodeChunk(std::string_view& data, std::size_t availableCapacity, char* buf) = 0;

  // Finalize the encoding stream, writing any remaining bytes into 'buf'.
  // May require multiple calls until it returns 0. Further calls after 0 are undefined.
  virtual EncoderResult end(std::size_t availableCapacity, char* buf) noexcept = 0;
};

}  // namespace aeronet