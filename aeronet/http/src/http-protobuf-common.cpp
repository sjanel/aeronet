#include "aeronet/http-protobuf-common.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/embedded-payload.hpp"
#include "aeronet/header-value-parameters.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-value-reverse-tokens-iterator.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/quality-value.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet {

namespace {

// application/protobuf and its deprecated aliases (RFC 9996 §7.1), the Google and Connect names, and the generic binary
// type.
constexpr std::string_view kBinaryMediaTypes[]{
    http::ContentTypeApplicationProtobuf, "application/x-protobuf", "application/x-protobuffer",
    "application/vnd.google.protobuf",    "application/proto",      http::ContentTypeApplicationOctetStream,
};

// The media type of ProtoJSON (RFC 9996 §7.2), sent with 'charset=utf-8'.
constexpr std::string_view kProtoJsonMediaType = "application/protobuf+json";
static_assert(http::ContentTypeApplicationProtobufJson.starts_with(kProtoJsonMediaType));

// application/protobuf+json and its deprecated alias (RFC 9996 §7.2), and the generic JSON type.
constexpr std::string_view kJsonMediaTypes[]{
    kProtoJsonMediaType,
    "application/x-protobuf+json",
    http::ContentTypeApplicationJson,
};

// Values of the Accept header of a 415 response: the canonical media types of the supported encodings.
constexpr std::string_view kMediaTypesSep = ", ";
constexpr std::string_view kAcceptBinary = http::ContentTypeApplicationProtobuf;
constexpr std::string_view kAcceptBinaryAndJson =
    JoinStringViewWithSep_v<kMediaTypesSep, http::ContentTypeApplicationProtobuf, kProtoJsonMediaType>;

// Bodies of a 406 response: the media types that can be sent (RFC 9110 §15.5.7).
constexpr std::string_view kNotAcceptablePrefix = "No acceptable media type for the protobuf response, available: ";
constexpr std::string_view kNotAcceptableBinary = JoinStringView_v<kNotAcceptablePrefix, kAcceptBinary>;
constexpr std::string_view kNotAcceptableBinaryAndJson = JoinStringView_v<kNotAcceptablePrefix, kAcceptBinaryAndJson>;

constexpr std::string_view kMaxBodySizeStr = IntToStringView_v<static_cast<int64_t>(kProtoMaxBodySize)>;

bool IsOneOf(std::string_view mediaType, std::span<const std::string_view> mediaTypes) noexcept {
  return std::ranges::any_of(
      mediaTypes, [mediaType](std::string_view candidate) { return CaseInsensitiveEqual(mediaType, candidate); });
}

// Whether a parameter of a media type of this format can be accepted (RFC 9996 §4, §5, §7.2). Other parameters are
// ignored.
bool IsAcceptedParameter(ProtoFormat format, std::string_view name, std::string_view value) noexcept {
  if (CaseInsensitiveEqual(name, "encoding")) {
    return CaseInsensitiveEqual(value, format == ProtoFormat::Binary ? "binary" : "json");
  }
  if (CaseInsensitiveEqual(name, "version")) {
    return value == "1";
  }
  return format == ProtoFormat::Binary || !CaseInsensitiveEqual(name, "charset") ||
         CaseInsensitiveEqual(value, "utf-8");
}

// A media range of an Accept header, matched against the media types of the protobuf encodings.
struct MediaRangeMatch {
  // How specific the media range is, the most specific one matching an encoding giving its quality (RFC 9110 §12.5.1):
  // -1 for no match, then '*/*', 'application/*', a media type of the encoding, and the same with parameters.
  enum Specificity : std::int8_t { kNone = -1, kAnyType, kAnySubtype, kMediaType, kMediaTypeWithParameters };

  // Records a media range matching the encoding with this specificity and quality.
  void update(Specificity rangeSpecificity, http::QualityValue rangeQuality) noexcept {
    if (rangeSpecificity > specificity) {
      specificity = rangeSpecificity;
      quality = rangeQuality;
    } else if (rangeSpecificity == specificity) {
      quality = std::max(quality, rangeQuality);  // the same range listed twice: the client accepts it at best
    }
  }

  http::QualityValue quality{0};  // 0 (not acceptable) until a media range matches
  Specificity specificity{kNone};
};

// Concatenates the parts with a single allocation.
std::string Concat(std::initializer_list<std::string_view> parts) {
  std::string ret;
  ret.reserve(std::ranges::fold_left(parts, std::size_t{0},
                                     [](std::size_t sum, std::string_view part) { return sum + part.size(); }));
  for (std::string_view part : parts) {
    ret.append(part);
  }
  return ret;
}

}  // namespace

ProtoFormat ProtoFormatOf(std::string_view contentType) noexcept {
  // A message without Content-Type may be treated as application/octet-stream (RFC 9110 §8.3).
  if (contentType.empty()) {
    return ProtoFormat::Binary;
  }
  http::HeaderParametersIterator parameters(contentType);
  const std::string_view mediaType = parameters.firstElement();
  ProtoFormat format;
  if (IsOneOf(mediaType, kBinaryMediaTypes)) {
    format = ProtoFormat::Binary;
  } else if (IsOneOf(mediaType, kJsonMediaTypes)) {
    format = ProtoFormat::Json;
  } else {
    return ProtoFormat::Unsupported;
  }

  // A parameter without '=' is ignored.
  while (parameters.hasNext()) {
    const http::HeaderParameter parameter = parameters.next();
    if (parameter.hasValue() && !IsAcceptedParameter(format, parameter.name, parameter.value)) {
      return ProtoFormat::Unsupported;
    }
  }
  return format;
}

ProtoFormat NegotiateProtoFormat(std::string_view accept, bool jsonSupported) noexcept {
  // Without Accept header, any media type is acceptable (RFC 9110 §12.5.1).
  if (accept.empty()) {
    return ProtoFormat::Binary;
  }
  MediaRangeMatch binary;
  MediaRangeMatch json;
  // Accept = #( media-range [ weight ] ), media-range = type "/" subtype *( OWS ";" OWS parameter ), weight being the
  // last parameter, 'q'. The order of the ranges does not matter.
  for (http::HeaderValueReverseTokensIterator it(accept); it.hasNext();) {
    const std::string_view range = it.next();
    http::HeaderParametersIterator parameters(range);
    const std::string_view mediaRange = parameters.firstElement();
    // The encodings that the range matches: both for a wildcard, one for a media type.
    bool matchesBinary = true;
    bool matchesJson = true;
    MediaRangeMatch::Specificity specificity = MediaRangeMatch::kMediaType;
    if (mediaRange == "*/*") {
      specificity = MediaRangeMatch::kAnyType;
    } else if (CaseInsensitiveEqual(mediaRange, "application/*")) {
      specificity = MediaRangeMatch::kAnySubtype;
    } else if (IsOneOf(mediaRange, kBinaryMediaTypes)) {
      matchesJson = false;
    } else if (IsOneOf(mediaRange, kJsonMediaTypes)) {
      matchesBinary = false;
    } else {
      continue;
    }

    http::QualityValue quality = http::kQualityValueMax;
    bool rejectedParameter = false;
    while (parameters.hasNext()) {
      const http::HeaderParameter parameter = parameters.next();
      if (!parameter.hasValue()) {
        continue;
      }
      if (CaseInsensitiveEqual(parameter.name, "q")) {
        quality = http::ParseQualityValue(parameter.value);
        break;  // the weight ends the media range
      }
      // The parameters of a wildcard are ignored: they cannot be checked against an encoding.
      if (specificity >= MediaRangeMatch::kMediaType) {
        const ProtoFormat format = matchesBinary ? ProtoFormat::Binary : ProtoFormat::Json;
        if (!IsAcceptedParameter(format, parameter.name, parameter.value)) {
          rejectedParameter = true;
          break;
        }
        specificity = MediaRangeMatch::kMediaTypeWithParameters;
      }
    }
    if (rejectedParameter) {
      continue;
    }
    if (matchesBinary) {
      binary.update(specificity, quality);
    }
    if (matchesJson) {
      json.update(specificity, quality);
    }
  }

  if (jsonSupported && json.quality > binary.quality) {
    return ProtoFormat::Json;
  }
  return binary.quality > 0 ? ProtoFormat::Binary : ProtoFormat::Unsupported;
}

http::StatusCode ProtoParseError::status() const noexcept {
  static constexpr http::StatusCode kStatusCodes[]{
      http::StatusCodeUnsupportedMediaType,  // UnsupportedMediaType
      http::StatusCodePayloadTooLarge,       // TooLarge
      http::StatusCodeBadRequest,            // Malformed
      http::StatusCodeBadRequest,            // MissingRequiredFields
  };
  return kStatusCodes[static_cast<std::uint8_t>(code)];
}

namespace internal {

ProtoParseError ProtoUnsupportedMediaTypeError(std::string_view contentType, bool jsonSupported) {
  return {
      .message = Concat({
          "Unsupported Content-Type '",
          contentType,
          "' for a protobuf body, expected ",
          jsonSupported ? "application/protobuf or application/protobuf+json" : http::ContentTypeApplicationProtobuf,
      }),
      .code = ProtoParseErrc::UnsupportedMediaType,
  };
}

ProtoParseError ProtoTooLargeError(std::size_t size) {
  return {
      .message = Concat(
          {"Protobuf body of ", std::to_string(size), " bytes exceeds the limit of ", kMaxBodySizeStr, " bytes"}),
      .code = ProtoParseErrc::TooLarge,
  };
}

ProtoParseError ProtoMalformedError(std::string_view typeName) {
  return {.message = Concat({"Invalid protobuf binary encoding of ", typeName}), .code = ProtoParseErrc::Malformed};
}

ProtoParseError ProtoJsonParseError(std::string_view typeName, std::string_view reason) {
  return {.message = Concat({"Invalid ProtoJSON for ", typeName, ": ", reason}), .code = ProtoParseErrc::Malformed};
}

ProtoParseError ProtoMissingRequiredFieldsError(std::string_view typeName, std::string_view missingFields) {
  return {
      .message = Concat({"Missing required fields of ", typeName, ": ", missingFields}),
      .code = ProtoParseErrc::MissingRequiredFields,
  };
}

HttpResponse ProtoParseErrorResponse(const HttpRequestView& request, ProtoParseError&& error, bool jsonSupported) {
  HttpResponse response = request.makeResponse(error.status());
  if (error.code == ProtoParseErrc::UnsupportedMediaType) {
    response.headerAddLine(http::Accept, jsonSupported ? kAcceptBinaryAndJson : kAcceptBinary);
  }
  response.body(std::move(error.message), http::ContentTypeTextPlain);
  return response;
}

ProtoResponseEncoding ProtoResponseEncodingFor(const HttpRequestView& request, bool jsonSupported,
                                               ProtoNotAcceptablePolicy policy) noexcept {
  const bool reject = policy == ProtoNotAcceptablePolicy::Reject;
  if (!jsonSupported && !reject) {
    return {.format = ProtoFormat::Binary, .varyAccept = false};  // whatever the Accept header
  }
  const ProtoFormat format = NegotiateProtoFormat(request.headerValueOrEmpty(http::Accept), jsonSupported);
  return {.format = format == ProtoFormat::Unsupported && !reject ? ProtoFormat::Binary : format, .varyAccept = true};
}

HttpResponse ProtoResponseHead(const HttpRequestView& request, http::StatusCode statusCode, std::size_t bodySize,
                               ProtoResponseEncoding encoding) {
  assert(encoding.format != ProtoFormat::Unsupported);
  const bool inlineBody = encoding.format == ProtoFormat::Binary;
  std::size_t additionalCapacity = HttpMessage::BodySize(bodySize, ProtoContentType(encoding.format).size());
  if (encoding.varyAccept) {
    additionalCapacity += http::HeaderSize(http::Vary.size(), http::Accept.size());
  }
  if (!inlineBody) {
    additionalCapacity -= bodySize;
  }
  // A captured body, and the size of a HEAD body, are embedded in the buffer (see HttpMessage::bodyInlineSet()).
  if (!inlineBody || request.method() == http::Method::HEAD) {
    additionalCapacity += EmbeddedPayload::kMaxFootprint;
  }
  HttpResponse response = request.makeResponse(additionalCapacity, statusCode);
  if (encoding.varyAccept) {
    response.headerAddLine(http::Vary, http::Accept);
  }
  return response;
}

HttpResponse ProtoNotAcceptableResponse(const HttpRequestView& request, bool jsonSupported) {
  const std::string_view body = jsonSupported ? kNotAcceptableBinaryAndJson : kNotAcceptableBinary;
  const std::size_t additionalCapacity =
      http::HeaderSize(http::Vary.size(), http::Accept.size()) + HttpMessage::BodySize(body.size());
  HttpResponse response = request.makeResponse(additionalCapacity, http::StatusCodeNotAcceptable);
  response.headerAddLine(http::Vary, http::Accept);
  response.body(body);
  return response;
}

void ThrowProtoTooLarge(std::size_t size) {
  throw std::length_error(Concat({
      "Protobuf message of ",
      std::to_string(size),
      " bytes exceeds the limit of its binary encoding, ",
      kMaxBodySizeStr,
      " bytes",
  }));
}

void ThrowProtoJsonSerializeError(std::string_view typeName, std::string_view reason) {
  throw std::runtime_error(Concat({"ProtoJSON serialization of ", typeName, " failed: ", reason}));
}

}  // namespace internal

}  // namespace aeronet
