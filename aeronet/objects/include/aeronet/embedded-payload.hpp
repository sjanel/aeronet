#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "aeronet/http-payload.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Low-level helpers storing an HttpPayload object inside a RawChars buffer, right after its first 'headSize' bytes:
//   [head: headSize bytes][padding: < alignof(HttpPayload)][HttpPayload object]
// When a payload is embedded, buf.size() == Offset(headSize) + sizeof(HttpPayload).
// This spares a 64 bytes HttpPayload member to the objects carrying a message buffer (HttpMessage, HttpMessageData),
// and the payload object never moves when its owner is moved (only the buffer pointer is transferred). The owner tracks
// whether a payload is embedded and the head size. While a payload is embedded, the buffer must never be reallocated
// nor written after the head: RelocateOut / RelocateIn allow to temporarily extract the payload.
class EmbeddedPayload {
 public:
  static constexpr std::size_t kAlign = alignof(HttpPayload);

  // Upper bound of the number of bytes needed after the head to host the padding + the HttpPayload object.
  static constexpr std::size_t kMaxFootprint = (kAlign - 1U) + sizeof(HttpPayload);

  // Offset of the payload object in a buffer whose head is 'headSize' bytes long.
  [[nodiscard]] static constexpr std::size_t Offset(std::size_t headSize) noexcept {
    return (headSize + kAlign - 1U) & ~(kAlign - 1U);
  }

  // Number of bytes needed after a head of 'headSize' bytes to host the padding + the HttpPayload object.
  [[nodiscard]] static constexpr std::size_t Footprint(std::size_t headSize) noexcept {
    return Offset(headSize) - headSize + sizeof(HttpPayload);
  }

  // Returns the payload embedded after the first 'headSize' bytes of 'buf'. Precondition: a payload is embedded.
  [[nodiscard]] static const HttpPayload* Get(const RawChars& buf, std::size_t headSize) noexcept {
    assert(reinterpret_cast<std::uintptr_t>(buf.data()) % kAlign == 0);
    assert(buf.size() == Offset(headSize) + sizeof(HttpPayload));
    // launder: the object was created by placement new inside a byte buffer
    return std::launder(reinterpret_cast<const HttpPayload*>(buf.data() + Offset(headSize)));
  }

  [[nodiscard]] static HttpPayload* Get(RawChars& buf, std::size_t headSize) noexcept {
    return const_cast<HttpPayload*>(Get(std::as_const(buf), headSize));
  }

  // Constructs a HttpPayload from 'args' right after the first 'headSize' bytes of 'buf', whose size must be 'headSize'
  // (no payload embedded). The buffer is grown if needed - reserve kMaxFootprint bytes in advance to avoid it.
  // 'buf' is left unmodified if this throws.
  template <class... Args>
  static HttpPayload* Construct(RawChars& buf, std::size_t headSize, Args&&... args) {
    assert(buf.size() == headSize);
    buf.ensureAvailableCapacity(Footprint(headSize));
    const std::size_t offset = Offset(headSize);
    HttpPayload* pPayload =
        std::construct_at(reinterpret_cast<HttpPayload*>(buf.data() + offset), std::forward<Args>(args)...);
    buf.setSize(offset + sizeof(HttpPayload));
    return pPayload;
  }

  // Destroys the embedded payload and shrinks 'buf' back to its head.
  static void Destroy(RawChars& buf, std::size_t headSize) noexcept {
    std::destroy_at(Get(buf, headSize));
    buf.setSize(headSize);
  }

  // Moves the embedded payload out, destroys it and shrinks 'buf' back to its head.
  [[nodiscard]] static HttpPayload Release(RawChars& buf, std::size_t headSize) noexcept {
    HttpPayload* pPayload = Get(buf, headSize);
    HttpPayload ret(std::move(*pPayload));
    std::destroy_at(pPayload);
    buf.setSize(headSize);
    return ret;
  }

  // Relocates the embedded payload into 'storage' (uninitialized, at least sizeof(HttpPayload) bytes aligned on
  // kAlign), and shrinks 'buf' back to its head. The buffer can then be reallocated and its head modified, before
  // RelocateIn puts the payload back.
  static void RelocateOut(RawChars& buf, std::size_t headSize, void* storage) noexcept {
    Relocate(Get(buf, headSize), storage);
    buf.setSize(headSize);
  }

  // Relocates the payload object living in 'storage' (filled by RelocateOut) right after the first 'headSize' bytes of
  // 'buf'. Precondition: buf.size() == headSize, and its capacity can host the payload.
  static void RelocateIn(RawChars& buf, std::size_t headSize, void* storage) noexcept {
    assert(buf.size() == headSize);
    const std::size_t offset = Offset(headSize);
    assert(buf.capacity() >= offset + sizeof(HttpPayload));
    Relocate(std::launder(reinterpret_cast<HttpPayload*>(storage)), buf.data() + offset);
    buf.setSize(offset + sizeof(HttpPayload));
  }

 private:
  static_assert(std::has_single_bit(kAlign));
  // Alignment is computed from the offset only: this relies on RawChars buffers being malloc / realloc'ed.
  static_assert(kAlign <= alignof(std::max_align_t));
  static_assert(std::is_nothrow_move_constructible_v<HttpPayload>);
  static_assert(std::is_nothrow_destructible_v<HttpPayload>);

  // Relocates '*src' into 'dst': plain memcpy if its active alternative is trivially relocatable (the source object is
  // then not destroyed, its storage is simply reused), move + destroy otherwise (std::string with SSO for instance).
  static void Relocate(HttpPayload* src, void* dst) noexcept {
    if (src->isTriviallyRelocatable()) {
      std::memcpy(dst, static_cast<const void*>(src), sizeof(HttpPayload));
    } else {
      std::construct_at(static_cast<HttpPayload*>(dst), std::move(*src));
      std::destroy_at(src);
    }
  }
};

}  // namespace aeronet
