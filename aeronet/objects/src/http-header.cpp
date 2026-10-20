#include "aeronet/http-header.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/safe-cast.hpp"

namespace aeronet::http {

namespace {

// Tells whether str starts in the size bytes at pBuffer.
[[maybe_unused]] bool StartsIn(std::string_view str, const char* pBuffer, std::size_t size) noexcept {
  return reinterpret_cast<std::uintptr_t>(str.data()) - reinterpret_cast<std::uintptr_t>(pBuffer) < size;
}

}  // namespace

Header::Header(std::string_view name, std::string_view value)
    : _pData(static_cast<char*>(std::malloc(name.size() + value.size()))),
      _nameLength(SafeCast<uint32_t>(name.size())),
      _valueLength(SafeCast<uint32_t>(value.size())) {
  if (_pData == nullptr) {
    throw std::bad_alloc();
  }
  Copy(name, _pData);
  Copy(value, _pData + name.size());
}

Header::Header(Header&& rhs, std::string_view name, std::string_view value)
    : _pData(std::exchange(rhs._pData, nullptr)),
      _nameLength(SafeCast<uint32_t>(name.size())),
      _valueLength(SafeCast<uint32_t>(value.size())) {
  // The name may be the one of rhs (an HPACK dynamic table entry added with the name of the entry it evicts): its bytes
  // are already in place, and kept by realloc. Otherwise, name and value must not point into the buffer of rhs, which
  // realloc may free and which is overwritten.
  const bool nameInPlace = name.data() == _pData;
  assert(nameInPlace || name.empty() || !StartsIn(name, _pData, rhs.size()));
  assert(value.empty() || !StartsIn(value, _pData, rhs.size()));
  if (rhs.size() < size()) {
    char* pNewData = static_cast<char*>(std::realloc(_pData, name.size() + value.size()));
    if (pNewData == nullptr) {
      std::free(_pData);
      _pData = nullptr;
      throw std::bad_alloc();
    }
    _pData = pNewData;
  }
  if (!nameInPlace) {
    Copy(name, _pData);
  }
  Copy(value, _pData + name.size());
}

Header::Header(const Header& rhs)
    : _pData(static_cast<char*>(std::malloc(rhs.size()))),
      _nameLength(rhs._nameLength),
      _valueLength(rhs._valueLength) {
  if (rhs._pData != nullptr) {
    if (_pData == nullptr) {
      throw std::bad_alloc();
    }
    Copy(rhs._pData, rhs.size(), _pData);
  }
}

Header::Header(Header&& rhs) noexcept
    : _pData(std::exchange(rhs._pData, nullptr)),
      _nameLength(std::exchange(rhs._nameLength, 0)),
      _valueLength(std::exchange(rhs._valueLength, 0)) {}

Header& Header::operator=(const Header& rhs) {
  if (this != &rhs) [[likely]] {
    const auto newSize = rhs.size();
    if (newSize > size()) {
      char* pNewData = static_cast<char*>(std::realloc(_pData, newSize));
      if (pNewData == nullptr) {
        throw std::bad_alloc();
      }
      _pData = pNewData;
    }
    _nameLength = rhs._nameLength;
    _valueLength = rhs._valueLength;
    Copy(rhs._pData, newSize, _pData);
  }
  return *this;
}

Header& Header::operator=(Header&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    std::free(_pData);
    _pData = std::exchange(rhs._pData, nullptr);
    _nameLength = std::exchange(rhs._nameLength, 0);
    _valueLength = std::exchange(rhs._valueLength, 0);
  }
  return *this;
}

Header::~Header() { std::free(_pData); }

}  // namespace aeronet::http