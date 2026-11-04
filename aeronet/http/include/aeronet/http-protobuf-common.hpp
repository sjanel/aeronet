#pragma once

// The parts of the Protocol Buffers body helpers (<aeronet/http-protobuf.hpp>) that do not depend on protobuf: the
// media types of a protobuf body and its parsing errors. They are compiled in the aeronet libraries, which never
// include protobuf headers, so they do not depend on the version of protobuf.

#ifdef AERONET_ENABLE_PROTOBUF

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "aeronet/http-status-code.hpp"

namespace aeronet {

class HttpRequestView;
class HttpResponse;

// Largest protobuf binary encoding: its size is an int in the protobuf runtime.
inline constexpr std::size_t kProtoMaxBodySize = static_cast<std::size_t>(std::numeric_limits<int>::max());

// Encoding of a protobuf body, deduced from its Content-Type (RFC 9996).
enum class ProtoFormat : std::uint8_t {
  // application/protobuf, its aliases application/x-protobuf, application/x-protobuffer,
  // application/vnd.google.protobuf and application/proto, application/octet-stream, or no Content-Type.
  Binary,
  // ProtoJSON: application/protobuf+json, its alias application/x-protobuf+json, or application/json.
  Json,
  // Any other media type, or a parameter that RFC 9996 requires to reject: an 'encoding' that is not the one of the
  // media type, a 'version' other than 1, or a 'charset' other than utf-8 for ProtoJSON.
  Unsupported,
};

// Returns the encoding of a protobuf body with this Content-Type value (empty if the message has none).
// Media types and parameter names are case-insensitive.
[[nodiscard]] ProtoFormat ProtoFormatOf(std::string_view contentType) noexcept;

enum class ProtoParseErrc : std::uint8_t {
  UnsupportedMediaType,   // the Content-Type is not one of a protobuf encoding supported for the message
  TooLarge,               // the body exceeds kProtoMaxBodySize
  Malformed,              // invalid binary encoding or ProtoJSON
  MissingRequiredFields,  // proto2 'required' fields are absent
};

// Why a body could not be parsed as a protobuf message.
struct ProtoParseError {
  // Status of the response to a request with this body: 415 Unsupported Media Type, 413 Payload Too Large or 400 Bad
  // Request.
  [[nodiscard]] http::StatusCode status() const noexcept;

  std::string message;
  ProtoParseErrc code{ProtoParseErrc::Malformed};
};

namespace internal {

// Out-of-line cold paths of the templates of <aeronet/http-protobuf.hpp>.

[[nodiscard]] ProtoParseError ProtoUnsupportedMediaTypeError(std::string_view contentType, bool jsonSupported);

[[nodiscard]] ProtoParseError ProtoTooLargeError(std::size_t size);

[[nodiscard]] ProtoParseError ProtoMalformedError(std::string_view typeName);

[[nodiscard]] ProtoParseError ProtoJsonParseError(std::string_view typeName, std::string_view reason);

[[nodiscard]] ProtoParseError ProtoMissingRequiredFieldsError(std::string_view typeName,
                                                              std::string_view missingFields);

// The response to a request whose body could not be parsed: the status of 'error', its message as a text/plain body,
// and for an unsupported media type an Accept header listing the supported ones (RFC 9110 §15.5.16).
[[nodiscard]] HttpResponse ProtoParseErrorResponse(const HttpRequestView& request, ProtoParseError&& error,
                                                   bool jsonSupported);

[[noreturn]] void ThrowProtoTooLarge(std::size_t size);

[[noreturn]] void ThrowProtoJsonSerializeError(std::string_view typeName, std::string_view reason);

}  // namespace internal

}  // namespace aeronet

#endif  // AERONET_ENABLE_PROTOBUF
