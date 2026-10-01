#pragma once

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/embedded-payload.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class HttpResponse;

// A serialized HTTP message ready to be written to the transport: a head (status line, headers, and inline body /
// trailers if any), optionally followed by a captured body (HttpPayload).
// Like in HttpMessage, the captured body is not a member: it is constructed in place in the head buffer, right after
// the head (see EmbeddedPayload). This keeps this object small and trivially relocatable, the captured body never moves
// when this object is moved, and an HttpResponse can hand over its buffer as is.
class HttpMessageData {
 public:
  using trivially_relocatable = std::true_type;

  HttpMessageData() noexcept = default;

  explicit HttpMessageData(RawChars head) noexcept : _buf(std::move(head)), _headSize(_buf.size()) {}

  // Embeds 'body' right after 'head' (allocates if 'head' does not have enough spare capacity).
  HttpMessageData(RawChars head, HttpPayload body);

  HttpMessageData(const HttpMessageData&) = delete;
  HttpMessageData& operator=(const HttpMessageData&) = delete;

  HttpMessageData(HttpMessageData&& other) noexcept
      : _buf(std::move(other._buf)),
        _headSize(std::exchange(other._headSize, 0)),
        _offset(std::exchange(other._offset, 0)) {}

  HttpMessageData& operator=(HttpMessageData&& other) noexcept {
    if (this != &other) [[likely]] {
      if (hasPayload()) {
        destroyPayload();
      }
      _buf = std::move(other._buf);
      _headSize = std::exchange(other._headSize, 0);
      _offset = std::exchange(other._offset, 0);
    }
    return *this;
  }

  ~HttpMessageData() {
    if (hasPayload()) {
      destroyPayload();
    }
  }

  // Remaining bytes of the head to be written.
  [[nodiscard]] std::string_view firstBuffer() const noexcept {
    return _offset < _headSize ? std::string_view(_buf.data() + _offset, _headSize - _offset) : std::string_view();
  }

  // Remaining bytes of the captured body to be written (empty for a file payload).
  [[nodiscard]] std::string_view secondBuffer() const noexcept {
    if (!hasPayload()) {
      return {};
    }
    const std::string_view body = payload()->view();
    return _offset > _headSize ? body.substr(_offset - _headSize) : body;
  }

  [[nodiscard]] std::size_t remainingSize() const noexcept { return retainedSize() - _offset; }

  /// Logical bytes whose backing storage must stay alive if any part was submitted with MSG_ZEROCOPY.
  [[nodiscard]] std::size_t retainedSize() const noexcept {
    return _headSize + (hasPayload() ? payload()->size() : 0UL);
  }

  [[nodiscard]] bool empty() const noexcept { return remainingSize() == 0; }

  FilePayload* getIfFilePayload() noexcept { return hasPayload() ? payload()->getIfFilePayload() : nullptr; }

  void addOffset(std::size_t sz) noexcept { _offset += sz; }

  /// Grows the writable buffer by `sz` bytes and returns the start of the new uninitialized region.
  char* resizeUp(std::size_t sz);

  // Appends data to the message (to the captured body if any, to the head otherwise).
  void append(std::string_view sv);

  // Clears all data (destroying the captured body if any), keeping the capacity of the head buffer.
  void clear() noexcept;

  void shrink_to_fit();

 private:
  friend class HttpResponse;

  // Adopts 'buf' whose first 'headSize' bytes are the message head, followed by an embedded payload if
  // buf.size() != headSize (see EmbeddedPayload).
  HttpMessageData(RawChars buf, std::size_t headSize) noexcept : _buf(std::move(buf)), _headSize(headSize) {}

  [[nodiscard]] bool hasPayload() const noexcept { return _buf.size() != _headSize; }

  [[nodiscard]] const HttpPayload* payload() const noexcept { return EmbeddedPayload::Get(_buf, _headSize); }
  [[nodiscard]] HttpPayload* payload() noexcept { return EmbeddedPayload::Get(_buf, _headSize); }

  void destroyPayload() noexcept;

  RawChars _buf;
  // Number of message bytes at the start of _buf. A captured body is embedded right after them iff
  // _buf.size() != _headSize.
  std::size_t _headSize{};
  std::size_t _offset{};
};

}  // namespace aeronet
