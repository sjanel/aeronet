#include "aeronet/multipart-form-data.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {
namespace {

constexpr std::string_view kMultipartMediaType{"multipart/form-data"};

std::string_view Trim(std::string_view value) {
  auto begin = value.begin();
  auto end = value.end();
  while (begin != end && (*begin == ' ' || *begin == '\t')) {
    ++begin;
  }
  while (begin != end) {
    auto last = end;
    --last;
    if (*last == ' ' || *last == '\t') {
      end = last;
    } else {
      break;
    }
  }
  return value.substr(static_cast<std::size_t>(begin - value.begin()), static_cast<std::size_t>(end - begin));
}

std::string_view StripQuotes(std::string_view value) {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value.remove_prefix(1);
    value.remove_suffix(1);
  }
  return value;
}

struct ContentDispositionInfo {
  std::string_view name;
  std::optional<std::string_view> filename;
  std::string_view invalidReason;
};

ContentDispositionInfo ParseContentDisposition(std::string_view headerValue) {
  std::string_view trimmed = Trim(headerValue);
  ContentDispositionInfo ret;

  if (trimmed.empty()) {
    ret.invalidReason = "multipart part missing Content-Disposition value";
    return ret;
  }

  std::string_view type;

  bool firstToken = true;
  while (!trimmed.empty()) {
    auto semicolon = trimmed.find(';');
    std::string_view token = semicolon == std::string_view::npos ? trimmed : trimmed.substr(0, semicolon);
    token = Trim(token);
    if (token.empty()) {
      // Empty token (e.g., consecutive semicolons) is malformed
      ret.invalidReason = "multipart part invalid Content-Disposition parameter";
      return ret;
    }

    if (firstToken) {
      type = token;
    } else {
      auto eq = token.find('=');
      if (eq != std::string_view::npos) {
        std::string_view key = Trim(token.substr(0, eq));
        std::string_view value = Trim(token.substr(eq + 1));

        value = StripQuotes(value);
        if (CaseInsensitiveEqual(key, "name")) {
          ret.name = value;
        } else if (CaseInsensitiveEqual(key, "filename")) {
          ret.filename = value;
        } else if (CaseInsensitiveEqual(key, "filename*")) {
          // RFC 5987 style: charset'lang'value. We only support the simplest utf-8''value case.
          auto firstTick = value.find('\'');
          if (firstTick == std::string_view::npos) {
            ret.invalidReason = "multipart part invalid Content-Disposition filename* parameter";
            return ret;
          }
          auto secondTick = value.find('\'', firstTick + 1);
          // If the pattern isn't charset'lang'value (i.e., both ticks present and payload after), it's malformed.
          if (secondTick == std::string_view::npos || secondTick + 1 > value.size()) {
            ret.invalidReason = "multipart part invalid Content-Disposition filename* parameter";
            return ret;
          }
          ret.filename = value.substr(secondTick + 1);
        }
      } else {
        ret.invalidReason = "multipart part invalid Content-Disposition parameter";
        return ret;
      }
    }

    if (semicolon == std::string_view::npos) {
      break;
    }
    trimmed.remove_prefix(semicolon + 1);
    firstToken = false;
  }

  if (type.empty() || !CaseInsensitiveEqual(type, "form-data")) {
    ret.invalidReason = "multipart part must have Content-Disposition: form-data";
  } else if (ret.name.empty()) {
    ret.invalidReason = "multipart part missing name parameter";
  }
  return ret;
}

std::string_view ExtractBoundary(std::string_view contentType) {
  if (contentType.empty()) {
    return {};
  }

  auto semicolon = contentType.find(';');
  auto typeToken = semicolon == std::string_view::npos ? contentType : contentType.substr(0, semicolon);
  if (!CaseInsensitiveEqual(Trim(typeToken), kMultipartMediaType)) {
    return {};
  }

  if (semicolon == std::string_view::npos) {
    return {};
  }
  auto params = contentType.substr(semicolon + 1);
  while (!params.empty()) {
    if (params.front() == ';') {
      params.remove_prefix(1);
    }
    auto next = params.find(';');
    auto chunk = next == std::string_view::npos ? params : params.substr(0, next);
    auto eq = chunk.find('=');
    if (eq != std::string_view::npos) {
      auto key = Trim(chunk.substr(0, eq));
      auto value = Trim(chunk.substr(eq + 1));
      if (CaseInsensitiveEqual(key, "boundary")) {
        return StripQuotes(value);
      }
    }
    if (next == std::string_view::npos) {
      break;
    }
    params.remove_prefix(next + 1);
  }
  return {};
}

std::string_view AppendHeader(std::string_view line, const MultipartFormDataOptions& options, std::size_t headerCount,
                              vector<MultipartHeaderView>& headers) {
  auto colon = line.find(':');
  if (colon == std::string_view::npos) {
    return "multipart part header missing colon";
  }
  auto name = Trim(line.substr(0, colon));
  if (name.empty()) {
    return "multipart part header missing name";
  }
  auto value = Trim(line.substr(colon + 1));
  if (options.maxHeadersPerPart != 0 && headerCount >= options.maxHeadersPerPart) {
    return "multipart part exceeds header limit";
  }
  headers.emplace_back(name, value);
  return {};
}

std::size_t FindHeaderDelimiter(std::string_view buffer) {
  auto pos = buffer.find(http::DoubleCRLF);
  if (pos != std::string_view::npos) {
    return pos;
  }
  return buffer.find(http::CRLF);
}

std::size_t DelimiterLength(std::string_view buffer, std::size_t headerEnd) {
  if (headerEnd == std::string_view::npos) {
    return 0;
  }
  if (headerEnd + http::DoubleCRLF.size() <= buffer.size() &&
      buffer.substr(headerEnd, http::DoubleCRLF.size()) == http::DoubleCRLF) {
    return http::DoubleCRLF.size();
  }
  return http::CRLF.size();
}

}  // namespace

std::span<const MultipartHeaderView> MultipartFormData::Part::headers() const noexcept {
  if (headerCount == 0 || headerStore == nullptr) {
    return {};
  }
  return {headerStore->data() + headerOffset, headerCount};
}

std::string_view MultipartFormData::Part::headerValueOrEmpty(std::string_view key) const noexcept {
  const auto headers = this->headers();
  const auto it = std::ranges::find_if(
      headers, [&](const MultipartHeaderView& header) { return CaseInsensitiveEqual(header.name, key); });
  return it != headers.end() ? it->value : std::string_view{};
}

const MultipartFormData::Part* MultipartFormData::part(std::string_view name) const noexcept {
  const auto it = std::ranges::find_if(_parts, [name](const Part& part) { return part.name == name; });
  return it == _parts.end() ? nullptr : &*it;
}

vector<std::reference_wrapper<const MultipartFormData::Part>> MultipartFormData::parts(std::string_view name) const {
  vector<std::reference_wrapper<const Part>> matches;
  std::ranges::copy_if(_parts, std::back_inserter(matches), [&](const Part& part) { return part.name == name; });
  return matches;
}

MultipartFormData::MultipartFormData(std::string_view contentTypeHeader, std::string_view body,
                                     MultipartFormDataOptions options) {
  std::string_view boundaryView = ExtractBoundary(contentTypeHeader);
  if (boundaryView.empty()) {
    _invalidReason = "multipart/form-data boundary missing";
    return;
  }

  static constexpr std::string_view kDoubleDash{"--"};

  const auto matchFirstBoundary = [this, boundaryView](std::string_view& body) {
    if (!body.starts_with(kDoubleDash)) {
      _invalidReason = "multipart body missing starting boundary";
      return false;
    }
    body.remove_prefix(kDoubleDash.size());

    if (!body.starts_with(boundaryView)) {
      _invalidReason = "multipart body missing starting boundary";
      return false;
    }
    body.remove_prefix(boundaryView.size());
    return true;
  };

  static constexpr std::string_view kMiddleBoundaryPrefix{"\r\n--"};

  if (!matchFirstBoundary(body)) {
    return;
  }

  if (!body.starts_with(http::CRLF)) {
    _invalidReason = "multipart boundary not followed by CRLF";
    return;
  }
  body.remove_prefix(http::CRLF.size());

  while (true) {
    if (options.maxParts != 0 && _parts.size() >= options.maxParts) {
      _invalidReason = "multipart exceeds part limit";
      return;
    }

    auto headerEnd = FindHeaderDelimiter(body);
    if (headerEnd == std::string_view::npos) {
      _invalidReason = "multipart part missing header terminator";
      return;
    }
    auto headerBlock = body.substr(0, headerEnd);
    auto delimiterLen = DelimiterLength(body, headerEnd);
    body.remove_prefix(headerEnd + delimiterLen);

    auto& part = _parts.emplace_back();
    part.headerStore = &_headers;
    part.headerOffset = _headers.size();

    std::size_t headerCountForPart = 0;
    while (!headerBlock.empty()) {
      auto lineEnd = headerBlock.find(http::CRLF);
      std::string_view line;
      if (lineEnd == std::string_view::npos) {
        line = headerBlock;
        headerBlock = {};
      } else {
        line = headerBlock.substr(0, lineEnd);
        headerBlock.remove_prefix(lineEnd + http::CRLF.size());
      }
      if (line.empty()) {
        continue;
      }
      _invalidReason = AppendHeader(line, options, headerCountForPart, _headers);
      if (!_invalidReason.empty()) {
        return;
      }
      ++headerCountForPart;
    }
    part.headerCount = headerCountForPart;

    const auto partHeaders = part.headers();
    const auto contentDispositionIt = std::ranges::find_if(partHeaders, [&](const MultipartHeaderView& header) {
      return CaseInsensitiveEqual(header.name, http::ContentDisposition);
    });
    if (contentDispositionIt == partHeaders.end()) {
      _invalidReason = "multipart part missing Content-Disposition header";
      return;
    }
    const auto cdInfo = ParseContentDisposition(contentDispositionIt->value);
    if (!cdInfo.invalidReason.empty()) {
      _invalidReason = cdInfo.invalidReason;
      return;
    }
    part.name = cdInfo.name;
    part.filename = cdInfo.filename;

    if (auto contentType = part.headerValueOrEmpty(http::ContentType); !contentType.empty()) {
      part.contentType = contentType;
    }

    std::size_t boundaryPos = 0;
    bool found = false;

    while (true) {
      boundaryPos = body.find(kMiddleBoundaryPrefix, boundaryPos);
      if (boundaryPos == std::string_view::npos) {
        _invalidReason = "multipart part missing closing boundary";
        return;
      }
      if (body.substr(boundaryPos + kMiddleBoundaryPrefix.size()).starts_with(boundaryView)) {
        found = true;
        break;
      }
      boundaryPos += kMiddleBoundaryPrefix.size();
    }

    if (!found) {
      _invalidReason = "multipart part missing closing boundary";
      return;
    }

    if (options.maxPartSizeBytes != 0 && boundaryPos > options.maxPartSizeBytes) {
      _invalidReason = "multipart part exceeds size limit";
      return;
    }
    part.value = body.substr(0, boundaryPos);
    body.remove_prefix(boundaryPos + http::CRLF.size());  // drop CRLF preceding boundary marker

    if (!matchFirstBoundary(body)) {
      return;
    }

    bool finalBoundary = body.starts_with(kDoubleDash);
    if (finalBoundary) {
      body.remove_prefix(kDoubleDash.size());
    }

    if (body.starts_with(http::CRLF)) {
      body.remove_prefix(http::CRLF.size());
    } else if (!body.empty() && !finalBoundary) {
      _invalidReason = "multipart boundary missing CRLF";
      return;
    }

    if (finalBoundary) {
      if (!body.empty()) {
        if (body == http::CRLF) {
          body = {};
        } else {
          _invalidReason = "multipart data after final boundary";
          return;
        }
      }
      break;
    }
  }
}

}  // namespace aeronet
