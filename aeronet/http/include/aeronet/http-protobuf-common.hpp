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

#include "aeronet/http-constants.hpp"
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

// Chooses the encoding of a protobuf response from the Accept header value of the request (RFC 9110 §12.5.1), empty if
// the request has none: the binary encoding, or ProtoJSON if 'jsonSupported', whichever has the highest quality ('q').
// The media types of an encoding are those of ProtoFormatOf(), aliases included. The quality of an encoding is the one
// of the most specific media range that matches it: a media type with parameters, then without, then 'application/*',
// then '*/*'. A media type with a parameter that ProtoFormatOf() rejects ('version=2') matches no encoding.
// A tie goes to the binary encoding, as does an empty value. Returns Unsupported when neither encoding is acceptable:
// none is listed, or with a quality of 0.
[[nodiscard]] ProtoFormat NegotiateProtoFormat(std::string_view accept, bool jsonSupported) noexcept;

// What HttpRequestView::makeProtoResponse() sends when the Accept header of the request lists no media type of a
// supported encoding (see NegotiateProtoFormat()).
enum class ProtoNotAcceptablePolicy : std::uint8_t {
  // The binary encoding: a server may disregard the Accept header (RFC 9110 §12.5.1).
  SendBinary,
  // 406 Not Acceptable, with a text/plain body listing the supported media types (RFC 9110 §15.5.7).
  Reject,
};

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

// The encoding of the response of makeProtoResponse().
struct ProtoResponseEncoding {
  ProtoFormat format;  // Unsupported for a 406 response
  // Whether the encoding depends on the Accept header of the request ('Vary: Accept'): not for a message without
  // ProtoJSON with the policy SendBinary, always binary.
  bool varyAccept;
};

// The encoding of the response of makeProtoResponse() to 'request', for a message with ProtoJSON if 'jsonSupported'.
[[nodiscard]] ProtoResponseEncoding ProtoResponseEncodingFor(const HttpRequestView& request, bool jsonSupported,
                                                             ProtoNotAcceptablePolicy policy) noexcept;

// The Content-Type of a response body in this encoding (Binary or Json).
[[nodiscard]] constexpr std::string_view ProtoContentType(ProtoFormat format) noexcept {
  return format == ProtoFormat::Json ? http::ContentTypeApplicationProtobufJson : http::ContentTypeApplicationProtobuf;
}

// The response of makeProtoResponse() before its body, of 'bodySize' bytes in 'encoding' (Binary: inline, Json:
// captured), with a 'Vary: Accept' header if it varies: its buffer has room for the body headers, and for the body
// itself if inline, so that setting it does not reallocate.
[[nodiscard]] HttpResponse ProtoResponseHead(const HttpRequestView& request, http::StatusCode statusCode,
                                             std::size_t bodySize, ProtoResponseEncoding encoding);

// The 406 Not Acceptable response of makeProtoResponse(), with the policy ProtoNotAcceptablePolicy::Reject.
[[nodiscard]] HttpResponse ProtoNotAcceptableResponse(const HttpRequestView& request, bool jsonSupported);

[[noreturn]] void ThrowProtoTooLarge(std::size_t size);

[[noreturn]] void ThrowProtoJsonSerializeError(std::string_view typeName, std::string_view reason);

}  // namespace internal

}  // namespace aeronet

#endif  // AERONET_ENABLE_PROTOBUF
