#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace aeronet::http {

// Simple class holding a header name/value pair. The name and value are stored in a single contiguous dynamically
// allocated buffer, which is owned by the Header object. The name and value are stored in the buffer as a single
// string, with the name first, followed by the value. The name and value can be accessed via the name() and value()
// methods, which return string_views into the buffer. The size() method returns the total size of the header (name +
// value).
class Header {
 public:
  // Create an empty header entry (empty name and value). The buffer is not allocated until a non-empty name/value is
  // set.
  Header() noexcept = default;

  // Create a header entry with the specified name and value.
  Header(std::string_view name, std::string_view value);

  // Same as above, but by stealing the buffer of 'rhs'.
  // If it is smaller than the new name/value, the buffer will be reallocated.
  // name may be the name of rhs (its bytes are then kept in place); otherwise, name and value must not point into the
  // buffer of rhs.
  Header(Header&& rhs, std::string_view name, std::string_view value);

  // Header supports both move and copy semantics.
  Header(const Header&);
  Header(Header&& rhs) noexcept;
  Header& operator=(const Header&);
  Header& operator=(Header&& rhs) noexcept;

  ~Header();

  // Returns true if the header is empty (no name/value).
  [[nodiscard]] bool empty() const noexcept { return _pData == nullptr; }

  // Total size of the header, which is name.size() + value.size().
  [[nodiscard]] std::size_t size() const noexcept { return static_cast<std::size_t>(_nameLength) + _valueLength; }

  // Returns the name of the header as a string_view.
  [[nodiscard]] std::string_view name() const noexcept { return {_pData, _nameLength}; }

  // Returns the value of the header as a string_view.
  [[nodiscard]] std::string_view value() const noexcept { return {_pData + _nameLength, _valueLength}; }

  using trivially_relocatable = std::true_type;

 private:
  char* _pData{};
  uint32_t _nameLength{};
  uint32_t _valueLength{};
};

}  // namespace aeronet::http