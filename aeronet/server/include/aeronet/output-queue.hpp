#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "aeronet/http-message-data.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

// HTTP/1 output of a connection not written yet: the responses, or the parts of a streamed response, produced while
// earlier output waits for the socket (or batched, see internal::ConnectionIo::batchOutputOf()). They are written in
// order, several at once, the first entry possibly in part, and moved in rather than copied: neither them nor the
// first entry, which zerocopy sends may still read, are copied.
// The common case does not need it (output written at once): its storage is allocated on first use only.
class OutputQueue {
 public:
  // Data is copied at the end of the last entry, rather than queued on its own, when both are at most this large
  // together: small buffers (the events of a streamed response) are cheaper to copy than to write and release one by
  // one, and an entry grown this way copies at most this many bytes when reallocated.
  static constexpr std::size_t kMaxMergedSize = 4096;

  // Tells whether 'data' can be appended to 'last', an output that no zerocopy send reads: both are small, and neither
  // has a file payload (sent on its own, after the bytes of its message).
  [[nodiscard]] static bool CanMerge(const HttpMessageData& last, const HttpMessageData& data) noexcept {
    return last.retainedSize() + data.retainedSize() <= kMaxMergedSize && !last.hasFilePayload() &&
           !data.hasFilePayload();
  }

  [[nodiscard]] bool empty() const noexcept { return _first == _entries.size(); }

  // Number of queued entries.
  [[nodiscard]] std::size_t size() const noexcept { return _entries.size() - _first; }

  // The queued entries, in order. Only the first one may have been written, in part.
  [[nodiscard]] std::span<const HttpMessageData> entries() const noexcept { return {_entries.data() + _first, size()}; }

  // The first queued entry, the next one to write.
  [[nodiscard]] const HttpMessageData& front() const noexcept {
    assert(!empty());
    return _entries[_first];
  }

  // Total size of the queued entries not written yet (their file payloads excluded).
  [[nodiscard]] std::size_t bytes() const noexcept { return _bytes; }

  // Queues 'data' (not empty, offset 0): copied at the end of the last entry when CanMerge() allows it, unless that
  // entry is the first one and not 'frontAppendable' (zerocopy sends may read it), moved in otherwise.
  // Returns true if it was copied at the end of the last entry, false if queued as a new entry.
  bool push(HttpMessageData data, bool frontAppendable = true) {
    assert(!data.empty());
    _bytes += data.remainingSize();
    if (!empty() && (frontAppendable || size() > 1) && CanMerge(_entries.back(), data)) {
      _entries.back().append(std::move(data));
      return true;
    }
    if (_entries.size() == _entries.capacity() && _first != 0 && _first >= _entries.size() / 2) {
      // Reuses the room of the popped entries rather than growing: at least half of it, for amortized O(1) pushes.
      _entries.erase(_entries.begin(), _entries.begin() + _first);
      _first = 0;
    }
    _entries.push_back(std::move(data));
    return false;
  }

  // Records that the first 'nbBytes' unsent bytes of the first entry, fewer than all of them, were written.
  void frontWritten(std::size_t nbBytes) noexcept {
    assert(nbBytes < front().remainingSize());
    _entries[_first].addOffset(nbBytes);
    _bytes -= nbBytes;
  }

  // Removes the first entry and returns it.
  [[nodiscard]] HttpMessageData pop() noexcept {
    assert(_first < _entries.size());
    HttpMessageData data = std::move(_entries[_first]);
    _bytes -= data.remainingSize();
    if (++_first == _entries.size()) {
      _entries.clear();
      _first = 0;
    }
    return data;
  }

  // Drops all the entries (none of them is read by zerocopy sends), keeping the storage.
  void clear() noexcept {
    _entries.clear();
    _first = 0;
    _bytes = 0;
  }

  // Releases the storage when empty, otherwise the room of the popped entries and the unused capacity.
  void shrink_to_fit() {
    if (!empty()) {
      _entries.erase(_entries.begin(), _entries.begin() + _first);
      _first = 0;
    }
    _entries.shrink_to_fit();
  }

 private:
  // The popped entries (moved from) come first, before the queued ones.
  vector<HttpMessageData> _entries;
  std::size_t _bytes{};
  // Index of the first queued entry.
  std::uint32_t _first{};
};

}  // namespace aeronet
