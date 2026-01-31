#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aeronet {

class EncoderContext {
 public:
  virtual ~EncoderContext() = default;

  // Return the maximum compressed size for a given uncompressed size.
  // Only valid for sizing encodeChunk() output buffers (NOT for end()).
  [[nodiscard]] virtual std::size_t maxCompressedBytes(std::size_t uncompressedSize) const = 0;

  // Returns the minimal buffer size needed to hold data produced by a single end() call.
  [[nodiscard]] virtual std::size_t endChunkSize() const = 0;

  // Streaming chunk encoder.
  // You should not call encodeChunk() again after having finished the stream.
  // Returns -1 in case of error, otherwise the number of bytes written to 'buf' (0 is valid).
  virtual int64_t encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) = 0;

  // Finalize the encoding stream, writing any remaining bytes into 'buf'.
  // May require multiple calls until it returns 0. Further calls after 0 are undefined.
  // returns:
  //  >0 : bytes written
  //   0 : finished, no more output
  //  <0 : error
  virtual int64_t end(std::size_t availableCapacity, char* buf) noexcept = 0;
};

}  // namespace aeronet