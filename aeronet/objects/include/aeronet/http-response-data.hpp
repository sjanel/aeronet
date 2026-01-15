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

  explicit HttpResponseData(std::string_view data) : _headAndOptionalBody(data) {}

  explicit HttpResponseData(RawChars head) noexcept : _headAndOptionalBody(std::move(head)) {}

  HttpResponseData(RawChars head, HttpPayload body) noexcept
      : _headAndOptionalBody(std::move(head)), _capturedBody(std::move(body)) {}

  HttpResponseData(std::string_view head, HttpPayload body) noexcept
      : _headAndOptionalBody(head), _capturedBody(std::move(body)) {}

  [[nodiscard]] std::string_view firstBuffer() const noexcept {
    return _offset >= _headAndOptionalBody.size()
               ? std::string_view{}
               : std::string_view(_headAndOptionalBody.begin() + _offset, _headAndOptionalBody.end());
  }

  [[nodiscard]] std::string_view secondBuffer() const noexcept {
    return _offset > _headAndOptionalBody.size() ? _capturedBody.view().substr(_offset - _headAndOptionalBody.size())
                                                 : _capturedBody.view();
  }

  [[nodiscard]] std::size_t remainingSize() const noexcept {
    return _headAndOptionalBody.size() + _capturedBody.size() - _offset;
  }

  [[nodiscard]] bool empty() const noexcept { return remainingSize() == 0; }

  [[nodiscard]] const File &file() const noexcept { return _capturedBody.getIfFilePayload()->file; }
  [[nodiscard]] std::size_t fileLength() const noexcept { return _capturedBody.getIfFilePayload()->length; }
  [[nodiscard]] std::size_t fileOffset() const noexcept { return _capturedBody.getIfFilePayload()->offset; }

  auto *getIfFilePayload() noexcept { return _capturedBody.getIfFilePayload(); }

  [[nodiscard]] const auto *getIfFilePayload() const noexcept { return _capturedBody.getIfFilePayload(); }

  void addOffset(std::size_t sz) noexcept { _offset += sz; }

  void append(HttpResponseData other) {
    if (_capturedBody.empty()) {  // If our captured body is already set, we can only append other's data to it.
      _headAndOptionalBody.append(other._headAndOptionalBody);
      _capturedBody = std::move(other._capturedBody);
    } else {
      _capturedBody.append(other._headAndOptionalBody);
      _capturedBody.append(other._capturedBody);
    }
  }

  void append(std::string_view data) {
    if (_capturedBody.empty()) {
      _headAndOptionalBody.append(data);
    } else {
      _capturedBody.append(data);
    }
  }

  void clear() noexcept {
    _headAndOptionalBody.clear();
    _capturedBody.clear();
    _offset = 0;
  }

  void shrink_to_fit() {
    _headAndOptionalBody.shrink_to_fit();
    _capturedBody.shrink_to_fit();
  }

 private:
  RawChars _headAndOptionalBody;
  HttpPayload _capturedBody;
  std::size_t _offset{};
};

}  // namespace aeronet