#include "aeronet/test-transport.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

#include "aeronet/fault-policy.hpp"
#include "aeronet/test-pipe.hpp"
#include "aeronet/transport.hpp"

namespace aeronet::test {

namespace {

// Splitmix64 for seeding xoshiro256** state from a single seed.
uint64_t Splitmix64(uint64_t& state) {
  uint64_t result = (state += 0x9e3779b97f4a7c15ULL);
  result = (result ^ (result >> 30)) * 0xbf58476d1ce4e5b9ULL;
  result = (result ^ (result >> 27)) * 0x94d049bb133111ebULL;
  return result ^ (result >> 31);
}

uint64_t Rotl(uint64_t val, int shift) { return (val << shift) | (val >> (64 - shift)); }

}  // namespace

TestTransport::TestTransport(TestPipe& pipe, FaultPolicy policy) : _pipe(pipe), _policy(policy) {
  if (_policy.seed != 0) {
    uint64_t seedState = _policy.seed;
    _rngState[0] = Splitmix64(seedState);
    _rngState[1] = Splitmix64(seedState);
    _rngState[2] = Splitmix64(seedState);
    _rngState[3] = Splitmix64(seedState);
  }
}

uint64_t TestTransport::nextRandom() {
  // xoshiro256**
  const uint64_t result = Rotl(_rngState[1] * 5, 7) * 9;
  const uint64_t tmp = _rngState[1] << 17;
  _rngState[2] ^= _rngState[0];
  _rngState[3] ^= _rngState[1];
  _rngState[1] ^= _rngState[2];
  _rngState[0] ^= _rngState[3];
  _rngState[2] ^= tmp;
  _rngState[3] = Rotl(_rngState[3], 45);
  return result;
}

ITransport::TransportResult TestTransport::read(char* buf, std::size_t len) {
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

  // Check pipe state
  if (_pipe.isClientReset()) {
    return {0, TransportHint::Error};
  }

  std::size_t available = _pipe.serverReadAvailable();
  if (available == 0) {
    if (_pipe.isClientClosed()) {
      return {0, TransportHint::None};  // orderly close (EOF)
    }
    return {0, TransportHint::ReadReady};  // no data yet, would block
  }

  // Compute how many bytes to deliver this call
  std::size_t toRead = std::min(available, len);
  toRead = computeReadLimit(toRead);

  // Enforce total bytes threshold
  if (_totalBytesRead + toRead > _policy.resetAfterTotalBytesRead) {
    toRead = _policy.resetAfterTotalBytesRead - _totalBytesRead;
  }

  if (toRead == 0) {
    return {0, TransportHint::ReadReady};
  }

  // Read from pipe
  auto data = _pipe.serverRead(toRead);
  std::memcpy(buf, data.data(), data.size());
  _pipe.serverConsume(data.size());
  _totalBytesRead += data.size();

  return {data.size(), TransportHint::None};
}

ITransport::TransportResult TestTransport::write(std::string_view data) {
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

  // Check pipe state
  if (_pipe.isClientReset()) {
    return {0, TransportHint::Error};
  }

  if (_pipe.isClientClosed()) {
    return {0, TransportHint::Error};
  }

  std::size_t toWrite = data.size();

  // Apply partial write cap
  if (_policy.maxBytesPerWrite != 0) {
    if (_policy.seed != 0) {
      toWrite = std::min(toWrite, static_cast<std::size_t>((nextRandom() % _policy.maxBytesPerWrite) + 1));
    } else {
      toWrite = std::min(toWrite, _policy.maxBytesPerWrite);
    }
  }

  // Enforce total bytes threshold
  if (_totalBytesWritten + toWrite > _policy.resetAfterTotalBytesWritten) {
    toWrite = _policy.resetAfterTotalBytesWritten - _totalBytesWritten;
  }

  if (toWrite == 0) {
    return {0, TransportHint::WriteReady};
  }

  _pipe.serverWrite(data.substr(0, toWrite));
  _totalBytesWritten += toWrite;

  return {toWrite, TransportHint::None};
}

bool TestTransport::hasPendingReadData() const noexcept { return _pipe.serverReadAvailable() > 0; }

std::size_t TestTransport::computeReadLimit(std::size_t available) const {
  if (_policy.maxBytesPerRead == 0) {
    return available;
  }
  if (_policy.seed != 0) {
    // Use a const_cast-free approach: compute from read call count as deterministic variation
    // We can't call nextRandom() in a const method, so use a simple hash of state
    uint64_t hash = _readCallCount;
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
    hash ^= (hash >> 31);
    std::size_t limit = static_cast<std::size_t>((hash % _policy.maxBytesPerRead) + 1);
    return std::min(available, limit);
  }
  return std::min(available, _policy.maxBytesPerRead);
}

TestTransportPair MakeTestTransport(FaultPolicy policy) {
  TestTransportPair pair;
  pair.transport = std::make_unique<TestTransport>(pair.pipe, policy);
  return pair;
}

}  // namespace aeronet::test
