#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "aeronet/native-handle.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"

namespace aeronet::test {

// Plain transport over a non-blocking socket, counting the reads that would block before the first received byte: the
// response to a request takes a round trip at least, which the client waits for instead of trying a read first.
class FirstReadRecordingTransport final : public TransportBackend<FirstReadRecordingTransport> {
 public:
  explicit FirstReadRecordingTransport(NativeHandle fd) noexcept : _plain(fd) {}

  TransportResult read(char* buf, std::size_t len) {
    const TransportResult res = _plain.read(buf, len);
    if (_bytesRead == 0 && res.bytesProcessed == 0 && res.want == TransportHint::ReadReady) {
      ++_wouldBlockReadsBeforeData;
    }
    _bytesRead += res.bytesProcessed;
    return res;
  }

  TransportResult write(std::string_view data) { return _plain.write(data); }

  TransportResult write(std::string_view firstBuf, std::string_view secondBuf) {
    return _plain.write(firstBuf, secondBuf);
  }

  TransportResult write(std::span<const std::string_view> buffers) { return _plain.write(buffers); }

  [[nodiscard]] std::size_t wouldBlockReadsBeforeData() const noexcept { return _wouldBlockReadsBeforeData; }

 private:
  PlainTransport _plain;
  std::size_t _bytesRead{0};
  std::size_t _wouldBlockReadsBeforeData{0};
};

}  // namespace aeronet::test
