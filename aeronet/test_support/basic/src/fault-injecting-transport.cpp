#include "aeronet/fault-injecting-transport.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/fault-policy.hpp"
#include "aeronet/transport.hpp"

namespace aeronet::test {

FaultInjectingTransport::FaultInjectingTransport(std::unique_ptr<ITransport> inner, FaultPolicy policy)
    : _inner(std::move(inner)), _policy(policy) {}

ITransport::TransportResult FaultInjectingTransport::read(char* buf, std::size_t len) {
  ++_readCallCount;

  // Check one-shot reset
  if (_policy.resetOnNextRead) {
    _policy.resetOnNextRead = false;
    return {0, TransportHint::Error};
  }

  // Check total bytes threshold
  if (_totalBytesRead >= _policy.resetAfterTotalBytesRead) {
    return {0, TransportHint::Error};
  }

  // Check periodic EAGAIN
  if (_policy.eagainAfterEveryNReads != 0 && (_readCallCount % _policy.eagainAfterEveryNReads) == 0) {
    return {0, TransportHint::ReadReady};
  }

  // Apply partial read cap: limit how many bytes we request from the inner transport
  std::size_t maxRead = len;
  if (_policy.maxBytesPerRead != 0) {
    maxRead = std::min(len, _policy.maxBytesPerRead);
  }

  // Enforce total bytes threshold
  if (_totalBytesRead + maxRead > _policy.resetAfterTotalBytesRead) {
    maxRead = _policy.resetAfterTotalBytesRead - _totalBytesRead;
  }

  auto result = _inner->read(buf, maxRead);
  _totalBytesRead += result.bytesProcessed;
  return result;
}

ITransport::TransportResult FaultInjectingTransport::write(std::string_view data) {
  ++_writeCallCount;

  // Check one-shot reset
  if (_policy.resetOnNextWrite) {
    _policy.resetOnNextWrite = false;
    return {0, TransportHint::Error};
  }

  // Check total bytes threshold
  if (_totalBytesWritten >= _policy.resetAfterTotalBytesWritten) {
    return {0, TransportHint::Error};
  }

  // Check periodic EAGAIN
  if (_policy.eagainAfterEveryNWrites != 0 && (_writeCallCount % _policy.eagainAfterEveryNWrites) == 0) {
    return {0, TransportHint::WriteReady};
  }

  // Apply partial write cap
  std::string_view toWrite = data;
  if (_policy.maxBytesPerWrite != 0 && data.size() > _policy.maxBytesPerWrite) {
    toWrite = data.substr(0, _policy.maxBytesPerWrite);
  }

  // Enforce total bytes threshold
  if (_totalBytesWritten + toWrite.size() > _policy.resetAfterTotalBytesWritten) {
    toWrite = toWrite.substr(0, _policy.resetAfterTotalBytesWritten - _totalBytesWritten);
  }

  auto result = _inner->write(toWrite);
  _totalBytesWritten += result.bytesProcessed;
  return result;
}

ITransport::TransportResult FaultInjectingTransport::write(std::string_view firstBuf, std::string_view secondBuf) {
  // Delegate to single-buffer write to apply fault policies consistently
  return ITransport::write(firstBuf, secondBuf);
}

}  // namespace aeronet::test
