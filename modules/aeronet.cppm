// aeronet C++20 Module Interface
//
// This module re-exports the public API surface of <aeronet/aeronet.hpp>.
// Only user-facing types, configuration structs, handler callbacks, and
// protocol enums are exported. Internal implementation details (event loop,
// transport layer, encoders, HPACK, wire-protocol framing, etc.) are
// intentionally omitted — they are not part of the stable public API.
//
// Usage:
//   import aeronet;
//   using aeronet::HttpServer;

module;

#include <aeronet/aeronet.hpp>

export module aeronet;

export namespace aeronet {

// ── Core server types ──────────────────────────────────────────────────────

using aeronet::HttpServer;
using aeronet::MultiHttpServer;
using aeronet::SingleHttpServer;

// ── Request / Response primitives ──────────────────────────────────────────

using aeronet::ConcatenatedHeaders;
using aeronet::ConcatenatedHeaderValues;
using aeronet::FilePayload;
using aeronet::HeadersViewMap;
using aeronet::HttpPayload;
using aeronet::HttpRequest;
using aeronet::HttpResponse;
using aeronet::HttpResponseData;
using aeronet::HttpResponseWriter;
using aeronet::RequestTask;

// ── Routing & handlers ─────────────────────────────────────────────────────

using aeronet::AsyncRequestHandler;
using aeronet::MultipartFormData;
using aeronet::MultipartFormDataOptions;
using aeronet::RequestHandler;
using aeronet::RequestMiddleware;
using aeronet::ResponseMiddleware;
using aeronet::Router;
using aeronet::RouterConfig;
using aeronet::StaticFileConfig;
using aeronet::StaticFileHandler;
using aeronet::StreamingHandler;

// ── Configuration types ────────────────────────────────────────────────────

using aeronet::BuiltinProbesConfig;
using aeronet::CompressionConfig;
using aeronet::DecompressionConfig;
using aeronet::HttpServerConfig;
using aeronet::TelemetryConfig;
using aeronet::TLSConfig;
using aeronet::TLSInfo;
#ifdef AERONET_ENABLE_HTTP2
using aeronet::Http2Config;
#endif

// ── Encoding ───────────────────────────────────────────────────────────────

using aeronet::Encoding;
using aeronet::EncodingSelector;
using aeronet::GetEncodingStr;
using aeronet::IsEncodingEnabled;
using aeronet::kNbContentEncodings;

// ── Stats / metrics ────────────────────────────────────────────────────────

using aeronet::DogStatsD;
using aeronet::ServerStats;
using aeronet::SignalHandler;

// ── Version & feature flags ────────────────────────────────────────────────

using aeronet::brotliEnabled;
using aeronet::fullVersionStringView;
using aeronet::fullVersionWithRuntime;
using aeronet::http2Enabled;
using aeronet::MajorMinorVersion;
using aeronet::openSslEnabled;
using aeronet::openTelemetryEnabled;
using aeronet::spdLogEnabled;
using aeronet::version;
using aeronet::webSocketEnabled;
using aeronet::zlibEnabled;
using aeronet::zstdEnabled;

// ── Data types used in public API signatures ───────────────────────────────

using aeronet::ConcatenatedStrings;
using aeronet::ConcatenatedStrings32;
using aeronet::DynamicConcatenatedStrings;
using aeronet::File;
using aeronet::flat_hash_map;
using aeronet::ObjectArrayPool;
using aeronet::ObjectPool;
using aeronet::RawBytes;
using aeronet::RawBytes32;
using aeronet::RawChars;
using aeronet::RawChars32;
using aeronet::StaticConcatenatedStrings;

// ── String utilities (used in public type definitions) ─────────────────────

using aeronet::AppendIntegralToCharBuf;
using aeronet::CaseInsensitiveEqual;
using aeronet::CaseInsensitiveEqualFunc;
using aeronet::CaseInsensitiveHashFunc;
using aeronet::CaseInsensitiveLess;
using aeronet::CharToStringView;
using aeronet::CharToStringView_v;
using aeronet::IntegralToCharBuffer;
using aeronet::IntegralToCharVector;
using aeronet::IntToStringView;
using aeronet::IntToStringView_v;
using aeronet::JoinStringView;
using aeronet::JoinStringView_v;
using aeronet::JoinStringViewWithSep;
using aeronet::JoinStringViewWithSep_v;
using aeronet::make_joined_string_view;
using aeronet::SafeCast;
using aeronet::StringToIntegral;

// ── MIME utilities ─────────────────────────────────────────────────────────

using aeronet::DetermineMIMETypeIdx;
using aeronet::DetermineMIMETypeStr;
using aeronet::kMIMEMappings;
using aeronet::kUnknownMIMEMappingIdx;
using aeronet::MIMEMapping;
using aeronet::MIMETypeIdx;

// ── Time types ─────────────────────────────────────────────────────────────

using aeronet::SteadyClock;
using aeronet::SysClock;
using aeronet::SysDuration;
using aeronet::SysTimePoint;

// ── WebSocket (when enabled) ───────────────────────────────────────────────

#ifdef AERONET_ENABLE_WEBSOCKET
using aeronet::WebSocketEndpoint;
using aeronet::WebSocketHandlerFactory;
#endif

// ── JSON serialization (when glaze enabled) ────────────────────────────────

#ifdef AERONET_ENABLE_GLAZE
using aeronet::SerializeToJson;
#endif

// ── http:: sub-namespace — protocol enums, methods, status codes ───────────

namespace http {
// Method enum & helpers
using aeronet::http::IsMethodIdxSet;
using aeronet::http::IsMethodSet;
using aeronet::http::kAllMethodsStrLen;
using aeronet::http::kMethodStrings;
using aeronet::http::kNbMethods;
using aeronet::http::Method;
using aeronet::http::MethodBmp;
using aeronet::http::MethodBmpFromIdx;
using aeronet::http::MethodFromIdx;
using aeronet::http::MethodIdx;
using aeronet::http::MethodIdxToStr;
using aeronet::http::MethodToIdx;
using aeronet::http::MethodToStr;
using aeronet::http::operator|;

// Version
using aeronet::http::HTTP_1_0;
using aeronet::http::HTTP_1_1;
using aeronet::http::HTTP_2_0;
using aeronet::http::kHttpPrefix;
using aeronet::http::Version;

// Status code type & common codes
using aeronet::http::StatusCode;
using aeronet::http::StatusCodeAccepted;
using aeronet::http::StatusCodeBadGateway;
using aeronet::http::StatusCodeBadRequest;
using aeronet::http::StatusCodeConflict;
using aeronet::http::StatusCodeContinue;
using aeronet::http::StatusCodeCreated;
using aeronet::http::StatusCodeForbidden;
using aeronet::http::StatusCodeFound;
using aeronet::http::StatusCodeGatewayTimeout;
using aeronet::http::StatusCodeHTTPVersionNotSupported;
using aeronet::http::StatusCodeInternalServerError;
using aeronet::http::StatusCodeMethodNotAllowed;
using aeronet::http::StatusCodeMovedPermanently;
using aeronet::http::StatusCodeNoContent;
using aeronet::http::StatusCodeNotFound;
using aeronet::http::StatusCodeNotImplemented;
using aeronet::http::StatusCodeNotModified;
using aeronet::http::StatusCodeOK;
using aeronet::http::StatusCodePartialContent;
using aeronet::http::StatusCodePayloadTooLarge;
using aeronet::http::StatusCodePermanentRedirect;
using aeronet::http::StatusCodeRequestHeaderFieldsTooLarge;
using aeronet::http::StatusCodeRequestTimeout;
using aeronet::http::StatusCodeSeeOther;
using aeronet::http::StatusCodeServiceUnavailable;
using aeronet::http::StatusCodeSwitchingProtocols;
using aeronet::http::StatusCodeTemporaryRedirect;
using aeronet::http::StatusCodeTooManyRequests;
using aeronet::http::StatusCodeUnauthorized;
using aeronet::http::StatusCodeUnsupportedMediaType;

// Reason phrases
using aeronet::http::ReasonPhraseFor;

// Common header field names
using aeronet::http::AcceptEncoding;
using aeronet::http::AcceptRanges;
using aeronet::http::Allow;
using aeronet::http::CacheControl;
using aeronet::http::Connection;
using aeronet::http::ContentDisposition;
using aeronet::http::ContentEncoding;
using aeronet::http::ContentLength;
using aeronet::http::ContentRange;
using aeronet::http::ContentType;
using aeronet::http::Date;
using aeronet::http::ETag;
using aeronet::http::Expect;
using aeronet::http::Host;
using aeronet::http::IfMatch;
using aeronet::http::IfModifiedSince;
using aeronet::http::IfNoneMatch;
using aeronet::http::IfRange;
using aeronet::http::IfUnmodifiedSince;
using aeronet::http::LastModified;
using aeronet::http::Location;
using aeronet::http::Origin;
using aeronet::http::Range;
using aeronet::http::RetryAfter;
using aeronet::http::Trailer;
using aeronet::http::TransferEncoding;
using aeronet::http::Upgrade;
using aeronet::http::UserAgent;
using aeronet::http::Vary;

// CORS headers
using aeronet::http::AccessControlAllowCredentials;
using aeronet::http::AccessControlAllowHeaders;
using aeronet::http::AccessControlAllowMethods;
using aeronet::http::AccessControlAllowOrigin;
using aeronet::http::AccessControlAllowPrivateNetwork;
using aeronet::http::AccessControlExposeHeaders;
using aeronet::http::AccessControlMaxAge;
using aeronet::http::AccessControlRequestHeaders;
using aeronet::http::AccessControlRequestMethod;

// Method string constants
using aeronet::http::CONNECT;
using aeronet::http::DELETE;
using aeronet::http::GET;
using aeronet::http::HEAD;
using aeronet::http::OPTIONS;
using aeronet::http::PATCH;
using aeronet::http::POST;
using aeronet::http::PUT;
using aeronet::http::TRACE;

// Version strings
using aeronet::http::HTTP10Sv;
using aeronet::http::HTTP11Sv;

// Common header values
using aeronet::http::chunked;
using aeronet::http::close;
using aeronet::http::keepalive;

// Content types
using aeronet::http::ContentTypeApplicationJson;
using aeronet::http::ContentTypeApplicationOctetStream;
using aeronet::http::ContentTypeTextCss;
using aeronet::http::ContentTypeTextHtml;
using aeronet::http::ContentTypeTextJavascript;
using aeronet::http::ContentTypeTextPlain;

// Compression tokens
using aeronet::http::br;
using aeronet::http::deflate;
using aeronet::http::gzip;
using aeronet::http::identity;
using aeronet::http::zstd;

#ifdef AERONET_ENABLE_HTTP2
// HTTP/2 pseudo-headers
using aeronet::http::PseudoHeaderAuthority;
using aeronet::http::PseudoHeaderMethod;
using aeronet::http::PseudoHeaderPath;
using aeronet::http::PseudoHeaderScheme;
using aeronet::http::PseudoHeaderStatus;
#endif
}  // namespace http

// ── http2:: sub-namespace (when enabled) ───────────────────────────────────

#ifdef AERONET_ENABLE_HTTP2
namespace http2 {
// Core types
using aeronet::http2::ConnectionState;
using aeronet::http2::CreateHttp2ProtocolHandler;
using aeronet::http2::DataCallback;
using aeronet::http2::Http2Connection;
using aeronet::http2::Http2ProtocolHandler;
using aeronet::http2::Http2Stream;
using aeronet::http2::StreamEventCallback;

// Enums
using aeronet::http2::ErrorCode;
using aeronet::http2::FrameType;
using aeronet::http2::SettingsParameter;
using aeronet::http2::StreamState;

// Enum helpers
using aeronet::http2::ErrorCodeName;
using aeronet::http2::FrameTypeName;
using aeronet::http2::IsClientStream;
using aeronet::http2::IsServerStream;
using aeronet::http2::StreamStateName;

// Frame types & parsing (in umbrella via http2-frame.hpp)
using aeronet::http2::ComputeHeaderFrameFlags;
using aeronet::http2::ContinuationFrame;
using aeronet::http2::DataFrame;
using aeronet::http2::FrameHeader;
using aeronet::http2::FrameParseResult;
using aeronet::http2::GoAwayFrame;
using aeronet::http2::HeadersFrame;
using aeronet::http2::ParseContinuationFrame;
using aeronet::http2::ParseDataFrame;
using aeronet::http2::ParseFrameHeader;
using aeronet::http2::ParseGoAwayFrame;
using aeronet::http2::ParseHeadersFrame;
using aeronet::http2::ParsePingFrame;
using aeronet::http2::ParsePriorityFrame;
using aeronet::http2::ParseRstStreamFrame;
using aeronet::http2::ParseSettingsFrame;
using aeronet::http2::ParseWindowUpdateFrame;
using aeronet::http2::PingFrame;
using aeronet::http2::PriorityFrame;
using aeronet::http2::RstStreamFrame;
using aeronet::http2::SettingsEntry;
using aeronet::http2::SettingsFrame;
using aeronet::http2::WindowUpdateFrame;
using aeronet::http2::WriteContinuationFrame;
using aeronet::http2::WriteDataFrame;
using aeronet::http2::WriteFrame;
using aeronet::http2::WriteFrameHeader;
using aeronet::http2::WriteGoAwayFrame;
using aeronet::http2::WriteHeadersFrameWithPriority;
using aeronet::http2::WritePingFrame;
using aeronet::http2::WritePriorityFrame;
using aeronet::http2::WriteRstStreamFrame;
using aeronet::http2::WriteSettingsAckFrame;
using aeronet::http2::WriteSettingsFrame;
using aeronet::http2::WriteWindowUpdateFrame;

namespace FrameFlags {
using aeronet::http2::FrameFlags::ContinuationEndHeaders;
using aeronet::http2::FrameFlags::DataEndStream;
using aeronet::http2::FrameFlags::DataPadded;
using aeronet::http2::FrameFlags::HeadersEndHeaders;
using aeronet::http2::FrameFlags::HeadersEndStream;
using aeronet::http2::FrameFlags::HeadersPadded;
using aeronet::http2::FrameFlags::HeadersPriority;
using aeronet::http2::FrameFlags::PingAck;
using aeronet::http2::FrameFlags::SettingsAck;
}  // namespace FrameFlags

// Well-known constants
using aeronet::http2::kAlpnH2;
using aeronet::http2::kAlpnH2c;
using aeronet::http2::kConnectionPreface;
using aeronet::http2::kConnectionStreamId;
using aeronet::http2::kDefaultEnablePush;
using aeronet::http2::kDefaultHeaderTableSize;
using aeronet::http2::kDefaultInitialWindowSize;
using aeronet::http2::kDefaultMaxConcurrentStreams;
using aeronet::http2::kDefaultMaxFrameSize;
using aeronet::http2::kDefaultMaxHeaderListSize;
using aeronet::http2::kFrameHeaderSize;
using aeronet::http2::kMaxMaxFrameSize;
using aeronet::http2::kMaxStreamId;
using aeronet::http2::kMaxWindowSize;
using aeronet::http2::kMinMaxFrameSize;
}  // namespace http2
#endif

// ── tracing:: sub-namespace ────────────────────────────────────────────────

namespace tracing {
using aeronet::tracing::Span;
using aeronet::tracing::SpanPtr;
using aeronet::tracing::SpanRAII;
using aeronet::tracing::TelemetryContext;
}  // namespace tracing

// ── websocket:: sub-namespace (when enabled) ───────────────────────────────

#ifdef AERONET_ENABLE_WEBSOCKET
namespace websocket {
// User-facing types
using aeronet::websocket::CreateClientWebSocketHandler;
using aeronet::websocket::CreateServerWebSocketHandler;
using aeronet::websocket::DeflateConfig;
using aeronet::websocket::WebSocketCallbacks;
using aeronet::websocket::WebSocketConfig;
using aeronet::websocket::WebSocketHandler;

// Opcode enum & helpers
using aeronet::websocket::IsControlFrame;
using aeronet::websocket::IsDataFrame;
using aeronet::websocket::IsReservedOpcode;
using aeronet::websocket::Opcode;

// Close codes
using aeronet::websocket::CloseCode;

// Default config limits
using aeronet::websocket::kDefaultMaxFrameSize;
using aeronet::websocket::kDefaultMaxMessageSize;
}  // namespace websocket
#endif

}  // namespace aeronet
