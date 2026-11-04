#include "aeronet/http-protobuf-common.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet {

namespace {

// application/protobuf and its deprecated aliases (RFC 9996 §7.1), the Google and Connect names, and the generic binary
// type.
constexpr std::string_view kBinaryMediaTypes[] = {
    http::ContentTypeApplicationProtobuf, "application/x-protobuf", "application/x-protobuffer",
    "application/vnd.google.protobuf",    "application/proto",      http::ContentTypeApplicationOctetStream,
};

// application/protobuf+json and its deprecated alias (RFC 9996 §7.2), and the generic JSON type.
constexpr std::string_view kJsonMediaTypes[] = {
    "application/protobuf+json",
    "application/x-protobuf+json",
    http::ContentTypeApplicationJson,
};

// Values of the Accept header of a 415 response: the canonical media types of the supported encodings.
constexpr std::string_view kAcceptBinary = http::ContentTypeApplicationProtobuf;
constexpr std::string_view kAcceptBinaryAndJson = "application/protobuf, application/protobuf+json";

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

// Concatenates the parts with a single allocation.
std::string Concat(std::initializer_list<std::string_view> parts) {
  std::size_t size = 0;
  for (std::string_view part : parts) {
    size += part.size();
  }
  std::string ret;
  ret.reserve(size);
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
  std::size_t semicolonPos = contentType.find(';');
  const std::string_view mediaType = TrimOws(contentType.substr(0, semicolonPos));
  ProtoFormat format;
  if (IsOneOf(mediaType, kBinaryMediaTypes)) {
    format = ProtoFormat::Binary;
  } else if (IsOneOf(mediaType, kJsonMediaTypes)) {
    format = ProtoFormat::Json;
  } else {
    return ProtoFormat::Unsupported;
  }

  // Parameters: *( OWS ";" OWS [ name "=" value ] ). A parameter without '=' is ignored.
  while (semicolonPos != std::string_view::npos) {
    contentType.remove_prefix(semicolonPos + 1);
    semicolonPos = contentType.find(';');
    const std::string_view parameter = contentType.substr(0, semicolonPos);
    const std::size_t equalPos = parameter.find('=');
    if (equalPos != std::string_view::npos &&
        !IsAcceptedParameter(format, TrimOws(parameter.substr(0, equalPos)),
                             StripQuotes(TrimOws(parameter.substr(equalPos + 1))))) {
      return ProtoFormat::Unsupported;
    }
  }
  return format;
}

http::StatusCode ProtoParseError::status() const noexcept {
  static constexpr http::StatusCode kStatusCodes[] = {
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
