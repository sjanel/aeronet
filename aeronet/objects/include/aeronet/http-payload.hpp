#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Convenient wrapper of common user-types for HTTP body storage.
// The data is captured by value (moved or copied) at construction time.
// The body data is immutable after construction.
// The body view() accessor returns a std::string_view referencing the internal data.
class HttpPayload {
 private:
  using CharBuffer = std::pair<std::unique_ptr<char[]>, std::size_t>;
  using BytesBuffer = std::pair<std::unique_ptr<std::byte[]>, std::size_t>;

 public:
  HttpPayload() noexcept = default;

  // Constructs a HttpPayload by taking ownership of the given std::string.
  explicit HttpPayload(std::string str) noexcept : _data(std::move(str)) {}

  // Constructs a HttpPayload by taking ownership of the given std::vector<char>.
  explicit HttpPayload(std::vector<char> vec) noexcept : _data(std::move(vec)) {}

  // Constructs a HttpPayload by taking ownership of the given std::vector<std::byte>.
  explicit HttpPayload(std::vector<std::byte> vec) noexcept : _data(std::move(vec)) {}

  // Constructs a HttpPayload by taking ownership of the given buffer.
  explicit HttpPayload(std::unique_ptr<char[]> buf, std::size_t size) noexcept
      : _data(CharBuffer{std::move(buf), size}) {}

  // Constructs a HttpPayload by taking ownership of the given buffer.
  explicit HttpPayload(std::unique_ptr<std::byte[]> buf, std::size_t size) noexcept
      : _data(BytesBuffer{std::move(buf), size}) {}

  explicit HttpPayload(RawChars rawChars) noexcept : _data(std::move(rawChars)) {}

  HttpPayload(const HttpPayload&) = delete;
  HttpPayload(HttpPayload&&) noexcept = default;
  HttpPayload& operator=(const HttpPayload&) = delete;
  HttpPayload& operator=(HttpPayload&&) noexcept = default;

  ~HttpPayload() = default;

  [[nodiscard]] bool empty() const noexcept { return _data.index() == 0; }

  [[nodiscard]] std::size_t size() const noexcept;

  [[nodiscard]] char* data() noexcept;

  [[nodiscard]] std::string_view view() const noexcept;

  void append(std::string_view data);

  void append(const HttpPayload& other);

  void ensureAvailableCapacity(std::size_t capa);

  void ensureAvailableCapacityExponential(std::size_t capa);

  // Inserts bytes at position 'pos'.
  // May switch representation to RawChars for unsupported storage types.
  void insert(std::size_t pos, std::string_view data);

  // Should only be called after ensureAvailableCapacityExponential (capacity should be at least size() + sz)
  void addSize(std::size_t sz);

  void clear() noexcept;

  void shrink_to_fit();

 private:
  std::variant<std::monostate, std::string, std::vector<char>, std::vector<std::byte>, CharBuffer, BytesBuffer,
               RawChars>
      _data;
};

}  // namespace aeronet