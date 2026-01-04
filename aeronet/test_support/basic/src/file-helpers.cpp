#include "aeronet/file-helpers.hpp"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

#include "aeronet/file.hpp"

namespace aeronet {

std::string LoadAllContent(const File& file) {
  std::string content;
  content.resize_and_overwrite(file.size(), [&file](char* data, [[maybe_unused]] std::size_t newCap) {
    const auto readBytes = file.readAt(std::span<std::byte>(reinterpret_cast<std::byte*>(data), newCap), 0);
    if (readBytes == File::kError) {
      throw std::runtime_error("Failed to read file content");
    }
    return readBytes;
  });
  return content;
}

}  // namespace aeronet