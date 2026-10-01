#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/embedded-payload.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class HttpMessage;

// Data of an outgoing message: message bytes (typically a response head, possibly with its inline body), followed by an
// optional payload (captured body or file), and the offset of the first byte not sent yet.
// The payload is not a member: it is embedded in the spare capacity of the message bytes buffer (see EmbeddedPayload),
// which keeps this object small and lets HttpMessage hand over its buffer without relocating its payload.
class HttpMessageData {
 public:
  HttpMessageData() noexcept = default;

  explicit HttpMessageData(RawChars head) noexcept : _buf(std::move(head)) {}

  // Embeds 'body' (if not empty) after 'head', which is reallocated if its capacity is not enough.
  HttpMessageData(RawChars head, HttpPayload body) : _buf(std::move(head)) {
    if (!body.empty()) {
      const std::size_t msgSize = _buf.size();
      EmbeddedPayload::Emplace(_buf, std::move(body));
      setPayloadFootprint(_buf.size() - msgSize);
    }
  }

  HttpMessageData(const HttpMessageData&) = delete;
  HttpMessageData& operator=(const HttpMessageData&) = delete;

  HttpMessageData(HttpMessageData&& rhs) noexcept
      : _buf(std::move(rhs._buf)), _offsetBitmap(std::exchange(rhs._offsetBitmap, 0)) {}

  HttpMessageData& operator=(HttpMessageData&& rhs) noexcept {
    if (this != &rhs) [[likely]] {
      // Must happen BEFORE _buf is overwritten, which frees the storage of our payload.
      destroyPayload();
      _buf = std::move(rhs._buf);
      _offsetBitmap = std::exchange(rhs._offsetBitmap, 0);
    }
    return *this;
  }

  ~HttpMessageData() { destroyPayload(); }

  [[nodiscard]] std::string_view firstBuffer() const noexcept {
    const std::size_t msgSize = messageSize();
    const std::size_t offset = this->offset();
    return offset < msgSize ? std::string_view(_buf.data() + offset, msgSize - offset) : std::string_view();
  }

  [[nodiscard]] std::string_view secondBuffer() const noexcept {
    if (!hasPayload()) {
      return {};
    }
    const std::string_view payloadView = payload()->view();
    const std::size_t msgSize = messageSize();
    const std::size_t offset = this->offset();
    return offset > msgSize ? payloadView.substr(offset - msgSize) : payloadView;
  }

  [[nodiscard]] std::size_t remainingSize() const noexcept { return retainedSize() - offset(); }

  /// Logical bytes whose backing storage must stay alive if any part was submitted with MSG_ZEROCOPY.
  [[nodiscard]] std::size_t retainedSize() const noexcept {
    return messageSize() + (hasPayload() ? payload()->size() : 0UL);
  }

  [[nodiscard]] bool empty() const noexcept { return remainingSize() == 0; }

  FilePayload* getIfFilePayload() noexcept { return hasPayload() ? payload()->getIfFilePayload() : nullptr; }

  void addOffset(std::size_t sz) noexcept {
    assert(offset() + sz <= kOffsetMask);
    _offsetBitmap += sz;
  }

  // Appends the message bytes and the payload of 'other' (whose offset is ignored).
  void append(HttpMessageData other) {
    const std::string_view otherMsg(other._buf.data(), other.messageSize());
    if (hasPayload()) {
      // Our payload is already set: other's data can only be appended to it.
      HttpPayload* pPayload = payload();
      pPayload->append(otherMsg);
      if (other.hasPayload()) {
        pPayload->append(*other.payload());
      }
      return;
    }
    // Single allocation for other's message bytes and the storage of its payload, relocated (not copied) after them.
    _buf.ensureAvailableCapacity(otherMsg.size() + (other.hasPayload() ? EmbeddedPayload::kMaxFootprint : 0UL));
    _buf.unchecked_append(otherMsg);
    if (other.hasPayload()) {
      const std::size_t msgSize = _buf.size();
      EmbeddedPayload::RelocateNoRealloc(_buf, *other.payload());
      // The relocated payload is owned by this object now: 'other' must not destroy it.
      other.setPayloadFootprint(0);
      setPayloadFootprint(_buf.size() - msgSize);
    }
  }

  /// Grows the writable buffer by `sz` bytes and returns the start of the new uninitialized region.
  char* resizeUp(std::size_t sz) {
    if (!hasPayload()) {
      _buf.ensureAvailableCapacity(sz);
      char* pData = _buf.end();
      _buf.addSize(sz);
      return pData;
    }
    HttpPayload* pPayload = payload();
    const std::size_t oldSize = pPayload->size();
    pPayload->ensureAvailableCapacity(sz);
    pPayload->addSize(sz);
    return pPayload->data() + oldSize;
  }

  void append(std::string_view sv) {
    if (hasPayload()) {
      payload()->append(sv);
    } else {
      _buf.append(sv.data(), sv.size());
    }
  }

  void clear() noexcept {
    destroyPayload();
    _buf.clear();
    _offsetBitmap = 0;
  }

  void shrink_to_fit() {
    if (hasPayload()) {
      HttpPayload* pPayload = payload();
      pPayload->shrink_to_fit();
      if (!EmbeddedPayload::AllowsBufferReallocation(*pPayload)) {
        // Shrinking _buf would copy the payload object bitwise, which is not a valid relocation for it.
        return;
      }
    }
    _buf.shrink_to_fit();
  }

  // The payload lives in the heap buffer: moving this object around does not move it.
  using trivially_relocatable = std::true_type;

 private:
  friend class HttpMessage;

  // Takes over 'buf', made of 'msgSize' message bytes possibly followed by an embedded payload.
  HttpMessageData(RawChars buf, std::size_t msgSize) noexcept : _buf(std::move(buf)) {
    setPayloadFootprint(_buf.size() - msgSize);
  }

  // _offsetBitmap layout: [8 bits payload footprint][56 bits offset]
  // The payload footprint is the number of bytes at the end of _buf which are not message bytes (alignment padding +
  // embedded payload object), 0 if no payload is embedded.
  //
  // The 64-bit word is needed anyway for the offset, so packing the footprint in its upper bits does not grow this
  // object. A footprint is either 0, or padding (0 to EmbeddedPayload::kAlign - 1) + sizeof(HttpPayload), so in
  // [64, 71] with a 64-byte HttpPayload: std::bit_width(EmbeddedPayload::kMaxFootprint) = 7 bits would be enough.
  // A whole byte is used on purpose:
  //  - the offset keeps 56 bits (64 PiB), far beyond any message size, so the extra bit costs nothing.
  //  - the footprint is then the most significant byte of the word: compilers read it with a single byte load in
  //    messageSize(), and test it with a single byte compare in hasPayload(), instead of a 64-bit load + shift.
  //  - HttpPayload can grow up to 248 bytes without changing this layout (checked by the static_assert below).
  // Storing the whole footprint rather than only the padding and a presence bit (4 bits) keeps messageSize() a single
  // subtraction, without any branch.
  static constexpr std::uint32_t kPayloadFootprintShift = 56U;
  static constexpr std::uint64_t kOffsetMask = (std::uint64_t{1} << kPayloadFootprintShift) - 1U;

  static_assert(EmbeddedPayload::kMaxFootprint < (std::uint64_t{1} << (64U - kPayloadFootprintShift)));

  [[nodiscard]] std::size_t offset() const noexcept { return _offsetBitmap & kOffsetMask; }

  [[nodiscard]] bool hasPayload() const noexcept { return _offsetBitmap > kOffsetMask; }

  // Number of message bytes, at the start of _buf.
  [[nodiscard]] std::size_t messageSize() const noexcept {
    return _buf.size() - static_cast<std::size_t>(_offsetBitmap >> kPayloadFootprintShift);
  }

  void setPayloadFootprint(std::size_t footprint) noexcept {
    assert(footprint <= EmbeddedPayload::kMaxFootprint);
    _offsetBitmap = (_offsetBitmap & kOffsetMask) | (static_cast<std::uint64_t>(footprint) << kPayloadFootprintShift);
  }

  [[nodiscard]] const HttpPayload* payload() const noexcept {
    assert(hasPayload());
    return EmbeddedPayload::Get(_buf);
  }

  [[nodiscard]] HttpPayload* payload() noexcept { return const_cast<HttpPayload*>(std::as_const(*this).payload()); }

  void destroyPayload() noexcept {
    if (hasPayload()) {
      EmbeddedPayload::Destroy(_buf, messageSize());
      setPayloadFootprint(0);
    }
  }

  RawChars _buf;
  std::uint64_t _offsetBitmap{};
};

}  // namespace aeronet
