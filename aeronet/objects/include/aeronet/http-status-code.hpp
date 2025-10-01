#pragma once

#include <cstdint>

namespace aeronet::http {

using StatusCode = int16_t;

inline constexpr StatusCode StatusCodeOK = 200;
inline constexpr StatusCode StatusCodeContinue = 100;
inline constexpr StatusCode StatusCodeSwitchingProtocols = 101;
inline constexpr StatusCode StatusCodeProcessing = 102;
inline constexpr StatusCode StatusCodeEarlyHints = 103;

inline constexpr StatusCode StatusCodeCreated = 201;
inline constexpr StatusCode StatusCodeAccepted = 202;
inline constexpr StatusCode StatusCodeNonAuthoritativeInformation = 203;
inline constexpr StatusCode StatusCodeNoContent = 204;
inline constexpr StatusCode StatusCodeResetContent = 205;
inline constexpr StatusCode StatusCodePartialContent = 206;
inline constexpr StatusCode StatusCodeMultiStatus = 207;
inline constexpr StatusCode StatusCodeAlreadyReported = 208;
inline constexpr StatusCode StatusCodeIMUsed = 226;

inline constexpr StatusCode StatusCodeMultipleChoices = 300;
inline constexpr StatusCode StatusCodeMovedPermanently = 301;
inline constexpr StatusCode StatusCodeFound = 302;
inline constexpr StatusCode StatusCodeSeeOther = 303;
inline constexpr StatusCode StatusCodeNotModified = 304;
inline constexpr StatusCode StatusCodeUseProxy = 305;
inline constexpr StatusCode StatusCodeTemporaryRedirect = 307;
inline constexpr StatusCode StatusCodePermanentRedirect = 308;

inline constexpr StatusCode StatusCodeBadRequest = 400;
inline constexpr StatusCode StatusCodeUnauthorized = 401;
inline constexpr StatusCode StatusCodePaymentRequired = 402;
inline constexpr StatusCode StatusCodeForbidden = 403;
inline constexpr StatusCode StatusCodeNotFound = 404;
inline constexpr StatusCode StatusCodeMethodNotAllowed = 405;
inline constexpr StatusCode StatusCodeNotAcceptable = 406;
inline constexpr StatusCode StatusCodeProxyAuthenticationRequired = 407;
inline constexpr StatusCode StatusCodeRequestTimeout = 408;
inline constexpr StatusCode StatusCodeConflict = 409;
inline constexpr StatusCode StatusCodeGone = 410;
inline constexpr StatusCode StatusCodeLengthRequired = 411;
inline constexpr StatusCode StatusCodePreconditionFailed = 412;
inline constexpr StatusCode StatusCodePayloadTooLarge = 413;
inline constexpr StatusCode StatusCodeURITooLong = 414;
inline constexpr StatusCode StatusCodeUnsupportedMediaType = 415;
inline constexpr StatusCode StatusCodeRangeNotSatisfiable = 416;
inline constexpr StatusCode StatusCodeExpectationFailed = 417;
inline constexpr StatusCode StatusCodeImATeapot = 418;
inline constexpr StatusCode StatusCodeMisdirectedRequest = 421;
inline constexpr StatusCode StatusCodeUnprocessableEntity = 422;
inline constexpr StatusCode StatusCodeLocked = 423;
inline constexpr StatusCode StatusCodeFailedDependency = 424;
inline constexpr StatusCode StatusCodeTooEarly = 425;
inline constexpr StatusCode StatusCodeUpgradeRequired = 426;
inline constexpr StatusCode StatusCodePreconditionRequired = 428;
inline constexpr StatusCode StatusCodeTooManyRequests = 429;
inline constexpr StatusCode StatusCodeRequestHeaderFieldsTooLarge = 431;
inline constexpr StatusCode StatusCodeUnavailableForLegalReasons = 451;

inline constexpr StatusCode StatusCodeInternalServerError = 500;
inline constexpr StatusCode StatusCodeNotImplemented = 501;
inline constexpr StatusCode StatusCodeBadGateway = 502;
inline constexpr StatusCode StatusCodeServiceUnavailable = 503;
inline constexpr StatusCode StatusCodeGatewayTimeout = 504;
inline constexpr StatusCode StatusCodeHTTPVersionNotSupported = 505;
inline constexpr StatusCode StatusCodeVariantAlsoNegotiates = 506;
inline constexpr StatusCode StatusCodeInsufficientStorage = 507;
inline constexpr StatusCode StatusCodeLoopDetected = 508;
inline constexpr StatusCode StatusCodeNotExtended = 510;
inline constexpr StatusCode StatusCodeNetworkAuthenticationRequired = 511;

}  // namespace aeronet::http