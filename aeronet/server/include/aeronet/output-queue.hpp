#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "aeronet/http-message-data.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

// HTTP/1 output of a connection queued behind the output it is writing (ConnectionState::outBuffer, then the file sent
// after it): the responses, or the parts of a streamed response, produced while earlier output waits for the socket.
// They are written in order, several at once, and moved in rather than copied: neither them nor the output ahead of
// them, which zerocopy sends may still read, are copied.
// The common case does not need it (output written at once, or waiting alone in outBuffer): its storage is allocated on
// first use only, keeping it a single pointer in ConnectionState.
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

  [[nodiscard]] bool empty() const noexcept {
    return _pStorage == nullptr || _pStorage->first == _pStorage->entries.size();
  }

  // The queued entries, in order. None of them was written yet.
  [[nodiscard]] std::span<const HttpMessageData> entries() const noexcept {
    if (_pStorage == nullptr) {
      return {};
    }
    return {_pStorage->entries.data() + _pStorage->first, _pStorage->entries.size() - _pStorage->first};
  }

  // Total size of the queued entries (their file payloads excluded).
  [[nodiscard]] std::size_t bytes() const noexcept { return _pStorage == nullptr ? 0 : _pStorage->bytes; }

  // Queues 'data' (offset 0): copied at the end of the last entry when CanMerge() allows it, moved in otherwise.
  // Returns true if it was copied at the end of the last entry, false if queued as a new entry.
  bool push(HttpMessageData data) {
    if (_pStorage == nullptr) {
      _pStorage = std::make_unique<Storage>();
    }
    Storage& storage = *_pStorage;
    storage.bytes += data.remainingSize();
    if (!empty() && CanMerge(storage.entries.back(), data)) {
      storage.entries.back().append(std::move(data));
      return true;
    }
    if (storage.entries.size() == storage.entries.capacity() && storage.first != 0 &&
        storage.first >= storage.entries.size() / 2) {
      // Reuses the room of the popped entries rather than growing: at least half of it, for amortized O(1) pushes.
      storage.entries.erase(storage.entries.begin(), storage.entries.begin() + storage.first);
      storage.first = 0;
    }
    storage.entries.push_back(std::move(data));
    return false;
  }

  // Removes the first entry and returns it.
  [[nodiscard]] HttpMessageData pop() noexcept {
    assert(!empty());
    Storage& storage = *_pStorage;
    HttpMessageData data = std::move(storage.entries[storage.first]);
    storage.bytes -= data.remainingSize();
    if (++storage.first == storage.entries.size()) {
      storage.entries.clear();
      storage.first = 0;
    }
    return data;
  }

  // Drops all the entries (none of them is read by a zerocopy send), keeping the storage.
  void clear() noexcept {
    if (_pStorage != nullptr) {
      _pStorage->entries.clear();
      _pStorage->first = 0;
      _pStorage->bytes = 0;
    }
  }

  // Releases the storage when empty, otherwise the room of the popped entries and the unused capacity.
  void shrink_to_fit() {
    if (empty()) {
      _pStorage.reset();
    } else {
      _pStorage->entries.erase(_pStorage->entries.begin(), _pStorage->entries.begin() + _pStorage->first);
      _pStorage->first = 0;
      _pStorage->entries.shrink_to_fit();
    }
  }

 private:
  struct Storage {
    // The popped entries (moved from) come first, before the queued ones.
    vector<HttpMessageData> entries;
    std::size_t bytes{};
    // Index of the first queued entry.
    std::uint32_t first{};
  };

  std::unique_ptr<Storage> _pStorage;
};

}  // namespace aeronet
