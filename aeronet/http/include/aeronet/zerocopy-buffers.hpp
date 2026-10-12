#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "aeronet/http-message-data.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy.hpp"

namespace aeronet {

// Output buffers of a connection written with MSG_ZEROCOPY. The kernel reads them after the writes returned, until the
// completion of their sends: each one is retained until then, in the order of the sends.
// Only a vector, to keep ConnectionState small: the retained size is summed when needed (by the writes eligible to
// MSG_ZEROCOPY, while buffers are retained), over few entries (bounded by maxZerocopyPendingBytes / zerocopyMinBytes).
class ZerocopyBuffers {
 public:
  // Retains 'data' until the completion of the zerocopy sends numbered before 'seqEnd' (those issued so far).
  void retain(HttpMessageData data, std::uint32_t seqEnd) {
    // Saturated: no retention limit reaches 4 GiB.
    const auto size = static_cast<std::uint32_t>(
        std::min<std::size_t>(data.retainedSize(), std::numeric_limits<std::uint32_t>::max()));
    _buffers.push_back(Entry{std::move(data), seqEnd, size});
  }

  // Releases the buffers whose sends completed.
  void releaseCompleted(const ZeroCopyState& sends) noexcept {
    auto it = _buffers.begin();
    while (it != _buffers.end() && sends.completedBefore(it->seqEnd)) {
      ++it;
    }
    _buffers.erase(_buffers.begin(), it);
  }

  // Calls 'fn' with each retained buffer (moved out), then forgets them.
  template <class Fn>
  void extract(Fn&& fn) {
    for (Entry& entry : _buffers) {
      fn(std::move(entry.data));
    }
    clear();
  }

  void clear() noexcept { _buffers.clear(); }

  void shrink_to_fit() { _buffers.shrink_to_fit(); }

  // Total size of the retained buffers.
  [[nodiscard]] std::size_t bytes() const noexcept {
    std::size_t total = 0;
    for (const Entry& entry : _buffers) {
      total += entry.size;
    }
    return total;
  }

  [[nodiscard]] std::size_t size() const noexcept { return _buffers.size(); }

  [[nodiscard]] bool empty() const noexcept { return _buffers.empty(); }

 private:
  struct Entry {
    // The payload of HttpMessageData lives in its heap buffer: moving it around does not move the bytes sent.
    using trivially_relocatable = std::true_type;

    HttpMessageData data;
    std::uint32_t seqEnd;
    // retainedSize() of data, saturated (in the padding of the entry, unlike reading it from data)
    std::uint32_t size;
  };

  vector<Entry> _buffers;
};

}  // namespace aeronet
