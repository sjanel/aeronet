#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "aeronet/file-payload.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class HttpMessage;

// Convenient wrapper of common user-types for HTTP body storage.
// The data is captured by value (moved or copied) at construction time.
// The body data is immutable after construction.
// The body view() accessor returns a std::string_view referencing the internal data.
class HttpPayload {
 private:
  using CharBuffer = std::pair<std::unique_ptr<char[]>, std::size_t>;
  using BytesBuffer = std::pair<std::unique_ptr<std::byte[]>, std::size_t>;

  // Owns a complete allocation while exposing only its initialized suffix. The HTTP/1.1 client uses this
  // representation to adopt a receive buffer without moving the response body over its parsed wire head.
  struct RawCharsSuffix {
    struct Free {
      void operator()(char* ptr) const noexcept;
    };

    RawCharsSuffix(RawChars rawChars, std::size_t suffixOffset) noexcept;

    RawCharsSuffix(RawCharsSuffix&& rhs) noexcept
        : buffer(std::move(rhs.buffer)), size(std::exchange(rhs.size, 0)), offset(std::exchange(rhs.offset, 0)) {}

    RawCharsSuffix& operator=(RawCharsSuffix&& rhs) noexcept {
      if (this != &rhs) {
        buffer = std::move(rhs.buffer);
        size = std::exchange(rhs.size, 0);
        offset = std::exchange(rhs.offset, 0);
      }
      return *this;
    }

    [[nodiscard]] std::string_view view() const noexcept {
      return size == 0 ? std::string_view{} : std::string_view(buffer.get() + offset, size);
    }

    std::unique_ptr<char, Free> buffer;
    std::size_t size{0};
    std::size_t offset;
  };

  static_assert(sizeof(RawCharsSuffix) <= sizeof(RawChars));

 public:
  constexpr HttpPayload() noexcept = default;

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

  explicit constexpr HttpPayload(std::string_view sv) noexcept : _data(sv) {}

  explicit HttpPayload(FilePayload filePayload) noexcept : _data(std::move(filePayload)) {}

  HttpPayload(const HttpPayload& rhs);
  HttpPayload& operator=(const HttpPayload& rhs);

  HttpPayload(HttpPayload&&) noexcept = default;
  HttpPayload& operator=(HttpPayload&&) noexcept = default;

  ~HttpPayload() = default;

  [[nodiscard]] constexpr bool empty() const noexcept { return _data.index() == 0; }

  [[nodiscard]] constexpr bool isFilePayload() const noexcept { return std::holds_alternative<FilePayload>(_data); }

  // Used only for HEAD responses where only the size matters.
  [[nodiscard]] constexpr bool isSizeOnly() const noexcept {
    if (auto sv = std::get_if<std::string_view>(&_data)) {
      return sv->data() == &kSizeOnlySentinel;
    }
    return false;
  }

  [[nodiscard]] constexpr bool hasCapturedBody() const noexcept { return _data.index() > 1U; }

  constexpr FilePayload* getIfFilePayload() noexcept { return std::get_if<FilePayload>(&_data); }

  [[nodiscard]] constexpr const FilePayload* getIfFilePayload() const noexcept {
    return std::get_if<FilePayload>(&_data);
  }

  // does not work for file payloads
  [[nodiscard]] std::size_t size() const noexcept;

  // does not work for file payloads
  [[nodiscard]] char* data() noexcept;

  // does not work for file payloads
  [[nodiscard]] std::string_view view() const noexcept;

  // Appends data to the body (internal or captured) from a `const char*` and size.
  void append(const char* data, std::size_t size);

  void append(std::string_view data) { append(data.data(), data.size()); }

  void append(const HttpPayload& other);

  void ensureAvailableCapacity(std::size_t capa);

  void ensureAvailableCapacityExponential(std::size_t capa);

  void ensureAvailableCapacityExponential(int64_t capa) {
    if (capa > 0) {
      ensureAvailableCapacityExponential(static_cast<std::size_t>(capa));
    }
  }

  // Inserts bytes at position 'pos'.
  // May switch representation to RawChars for unsupported storage types.
  void insert(std::size_t pos, std::string_view data);

  // Should only be called after ensureAvailableCapacity / ensureAvailableCapacityExponential (capacity should be at
  // least size() + sz)
  void addSize(std::size_t sz);

  void clear() noexcept;

  void shrink_to_fit();

  // Sentinel for HEAD responses where only the body size is tracked (data is never sent).
  static constexpr char kSizeOnlySentinel{};

 private:
  friend class HttpMessage;

  // Takes ownership of `rawChars` while exposing [suffixOffset, rawChars.size()) as the payload.
  HttpPayload(RawChars rawChars, std::size_t suffixOffset) noexcept
      : _data(RawCharsSuffix(std::move(rawChars), suffixOffset)) {}

  std::variant<std::monostate, FilePayload, std::string, std::string_view, std::vector<char>, std::vector<std::byte>,
               CharBuffer, BytesBuffer, RawChars, RawCharsSuffix>
      _data;
};

}  // namespace aeronet
