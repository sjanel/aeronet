#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "aeronet/vector.hpp"

namespace aeronet {

struct MultipartFormDataOptions {
  std::size_t maxParts{128};
  std::size_t maxHeadersPerPart{32};
  std::size_t maxPartSizeBytes{32ULL * 1024ULL * 1024ULL};
};

struct MultipartHeaderView {
  std::string_view name;
  std::string_view value;
};

class MultipartFormData {
 public:
  // Default constructor creates an empty MultipartFormData
  MultipartFormData() noexcept = default;

  // Parse multipart/form-data from the given content-type header and body without throwing on malformed input
  MultipartFormData(std::string_view contentTypeHeader, std::string_view body, MultipartFormDataOptions options = {});

  struct Part {
    explicit Part(const vector<MultipartHeaderView>& headerStoreRef)
        : headerOffset(headerStoreRef.size()), headerStore(headerStoreRef) {}

    std::string_view name;
    std::optional<std::string_view> filename;
    std::optional<std::string_view> contentType;
    std::string_view value;

    // Get all headers associated with this part
    [[nodiscard]] std::span<const MultipartHeaderView> headers() const noexcept {
      return {headerStore.data() + headerOffset, headerCount};
    }

    // Get the value of the specified header, or an empty string_view if not present
    [[nodiscard]] std::string_view headerValueOrEmpty(std::string_view key) const noexcept;

   private:
    friend class MultipartFormData;
    std::size_t headerOffset{0};
    std::size_t headerCount{0};
    const vector<MultipartHeaderView>& headerStore;
  };

  // Get all parsed parts
  [[nodiscard]] std::span<const Part> parts() const noexcept { return _parts; }

  // Check if any parts were parsed
  [[nodiscard]] bool empty() const noexcept { return _parts.empty(); }

  // Get the first part with the given name, or nullptr if not found
  [[nodiscard]] const Part* part(std::string_view name) const noexcept;

  // Get all parts with the given name
  [[nodiscard]] vector<std::reference_wrapper<const Part>> parts(std::string_view name) const;

  // Check if the MultipartFormData was successfully parsed
  [[nodiscard]] bool valid() const noexcept { return _invalidReason.empty(); }

  // If not valid(), get the reason for invalidity
  [[nodiscard]] std::string_view invalidReason() const noexcept { return _invalidReason; }

  using trivially_relocatable = std::true_type;

 private:
  vector<Part> _parts;
  vector<MultipartHeaderView> _headers;
  std::string_view _invalidReason;  // empty if valid
};

}  // namespace aeronet
