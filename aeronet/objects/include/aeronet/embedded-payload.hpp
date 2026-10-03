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

// Helpers to store an HttpPayload object in the spare capacity of a RawChars buffer, right after its message bytes,
// instead of as a separate (64 bytes) member of its owner.
// Layout: [message bytes][alignment padding][HttpPayload object], buf.size() covering all of them, so that the payload
// object always occupies the last sizeof(HttpPayload) bytes of the buffer.
// The owner keeps track of whether a payload is embedded, and of the size of its message bytes.
// The padding bytes are uninitialized and must never be sent.
// While a payload is embedded, the buffer must not be reallocated: a RawChars reallocation is a bitwise copy, which is
// not a valid relocation for all payload alternatives (e.g. a std::string using its small buffer).
class EmbeddedPayload {
 public:
  static constexpr std::size_t kAlign = alignof(HttpPayload);

  static_assert(std::has_single_bit(kAlign));
  // Alignment is computed from the OFFSET only, this relies on RawChars buffers being malloc/realloc'ed.
  static_assert(kAlign <= alignof(std::max_align_t));
  static_assert(std::is_nothrow_move_constructible_v<HttpPayload>);
  static_assert(std::is_nothrow_move_assignable_v<HttpPayload>);
  static_assert(std::is_nothrow_destructible_v<HttpPayload>);

  // Upper bound of the number of bytes needed after the message bytes to host the padding + the HttpPayload object.
  static constexpr std::size_t kMaxFootprint = (kAlign - 1U) + sizeof(HttpPayload);

  // Position of the payload object stored after 'msgSize' message bytes.
  static constexpr std::size_t Offset(std::size_t msgSize) noexcept { return (msgSize + kAlign - 1U) & ~(kAlign - 1U); }

  // Number of bytes needed after 'msgSize' message bytes to host the padding + the HttpPayload object.
  static constexpr std::size_t Footprint(std::size_t msgSize) noexcept {
    return Offset(msgSize) - msgSize + sizeof(HttpPayload);
  }

  // Returns the payload object stored at the end of 'buf'.
  [[nodiscard]] static const HttpPayload* Get(const RawChars& buf) noexcept {
    assert(buf.size() >= sizeof(HttpPayload));
    assert(reinterpret_cast<std::uintptr_t>(buf.end()) % kAlign == 0);
    // launder: the object was created by placement new inside a byte buffer
    return std::launder(reinterpret_cast<const HttpPayload*>(buf.end() - sizeof(HttpPayload)));
  }

  [[nodiscard]] static HttpPayload* Get(RawChars& buf) noexcept {
    return const_cast<HttpPayload*>(Get(std::as_const(buf)));
  }

  // Constructs a payload object from 'args' after the buf.size() message bytes of 'buf', whose capacity must already be
  // enough (see Footprint()). If the construction throws, 'buf' is left unmodified.
  template <class... Args>
  static HttpPayload* EmplaceNoRealloc(RawChars& buf,
                                       Args&&... args) noexcept(std::is_nothrow_constructible_v<HttpPayload, Args...>) {
    const std::size_t offset = Offset(buf.size());
    assert(offset + sizeof(HttpPayload) <= buf.capacity());
    HttpPayload* pPayload =
        std::construct_at(reinterpret_cast<HttpPayload*>(buf.data() + offset), std::forward<Args>(args)...);
    buf.setSize(offset + sizeof(HttpPayload));
    return pPayload;
  }

  // Same as EmplaceNoRealloc(), but first reserves the needed capacity. If it throws, 'buf' is left unmodified.
  static HttpPayload* Emplace(RawChars& buf, HttpPayload&& payload) {
    buf.ensureAvailableCapacity(Footprint(buf.size()));
    return EmplaceNoRealloc(buf, std::move(payload));
  }

  // Relocates 'src' into the uninitialized storage 'dst', ending the lifetime of 'src'.
  // A plain bitwise copy is used when the active alternative allows it.
  static void Relocate(HttpPayload& src, void* dst) noexcept {
    if (src.isTriviallyRelocatable()) {
      // Source object is not destroyed: its storage is simply reused (trivial relocation).
      std::memcpy(dst, static_cast<const void*>(&src), sizeof(HttpPayload));
    } else {
      std::construct_at(static_cast<HttpPayload*>(dst), std::move(src));
      std::destroy_at(&src);
    }
  }

  // Relocates 'src' after the buf.size() message bytes of 'buf', whose capacity must already be enough (see
  // Footprint()). The lifetime of 'src' ends.
  static void RelocateNoRealloc(RawChars& buf, HttpPayload& src) noexcept {
    const std::size_t offset = Offset(buf.size());
    assert(offset + sizeof(HttpPayload) <= buf.capacity());
    Relocate(src, buf.data() + offset);
    buf.setSize(offset + sizeof(HttpPayload));
  }

  // Tells whether the RawChars buffer hosting 'payload' can be reallocated (which copies it bitwise).
  [[nodiscard]] static bool AllowsBufferReallocation(const HttpPayload& payload) noexcept {
    return payload.isTriviallyRelocatable();
  }

  // Destroys the payload object stored at the end of 'buf', then truncates 'buf' to its 'msgSize' message bytes.
  static void Destroy(RawChars& buf, std::size_t msgSize) noexcept {
    assert(buf.size() == Offset(msgSize) + sizeof(HttpPayload));
    std::destroy_at(Get(buf));
    buf.setSize(msgSize);
  }

  // Same as Destroy(), but moves the payload out first.
  [[nodiscard]] static HttpPayload Release(RawChars& buf, std::size_t msgSize) noexcept {
    HttpPayload ret(std::move(*Get(buf)));
    Destroy(buf, msgSize);
    return ret;
  }
};

}  // namespace aeronet
