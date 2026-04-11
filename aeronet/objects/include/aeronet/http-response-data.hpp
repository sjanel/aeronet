#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

#include "aeronet/http-payload.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class HttpResponseData {
 public:
  HttpResponseData() noexcept = default;

  explicit HttpResponseData(std::string_view data) : _buf(data) {}

  explicit HttpResponseData(RawChars head) noexcept : _buf(std::move(head)) {}

  HttpResponseData(RawChars head, HttpPayload body) noexcept : _buf(std::move(head)), _capturedBody(std::move(body)) {}

  HttpResponseData(std::string_view head, HttpPayload body) noexcept : _buf(head), _capturedBody(std::move(body)) {}

  [[nodiscard]] std::string_view firstBuffer() const noexcept {
    return _offset >= _buf.size() ? std::string_view{} : std::string_view(_buf.begin() + _offset, _buf.end());
  }

  [[nodiscard]] std::string_view secondBuffer() const noexcept {
    return _offset > _buf.size() ? _capturedBody.view().substr(_offset - _buf.size()) : _capturedBody.view();
  }

  [[nodiscard]] std::size_t remainingSize() const noexcept { return _buf.size() + _capturedBody.size() - _offset; }

  [[nodiscard]] bool empty() const noexcept { return remainingSize() == 0; }

  [[nodiscard]] const File& file() const noexcept { return _capturedBody.getIfFilePayload()->file; }
  [[nodiscard]] std::size_t fileLength() const noexcept { return _capturedBody.getIfFilePayload()->length; }

  auto* getIfFilePayload() noexcept { return _capturedBody.getIfFilePayload(); }

  void addOffset(std::size_t sz) noexcept { _offset += sz; }

  void append(HttpResponseData other) {
    if (_capturedBody.empty()) {  // If our captured body is already set, we can only append other's data to it.
      _buf.append(other._buf);
      _capturedBody = std::move(other._capturedBody);
    } else {
      _capturedBody.append(other._buf);
      _capturedBody.append(other._capturedBody);
    }
  }

  void append(std::string_view data) {
    if (_capturedBody.empty()) {
      _buf.append(data.data(), data.size());
    } else {
      _capturedBody.append(data);
    }
  }

  void clear() noexcept {
    _buf.clear();
    _capturedBody.clear();
    _offset = 0;
  }

  void shrink_to_fit() {
    _buf.shrink_to_fit();
    _capturedBody.shrink_to_fit();
  }

 private:
  RawChars _buf;
  HttpPayload _capturedBody;
  std::size_t _offset{};
};

}  // namespace aeronet