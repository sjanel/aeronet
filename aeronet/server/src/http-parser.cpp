#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "aeronet/chunk-size-line.hpp"
#include "aeronet/compiler-config.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/header-line-parse.hpp"
#include "aeronet/header-merge.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/http1-pipeline.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/reserved-headers.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/search-crlf.hpp"
#include "aeronet/string-view-size-only-sentinel.hpp"
#include "aeronet/sv-to-sv-map.hpp"
#include "aeronet/tolower-str.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

namespace {

// Tells whether adding 'size' bytes to 'current' ones exceeds 'limit' (written so that it cannot overflow).
constexpr bool ExceedsLimit(std::size_t current, std::size_t size, std::size_t limit) noexcept {
  return size > limit || current > limit - size;
}

// Collects in 'chunks' the data of the chunks of the body in 'input' starting at the offset 'pos', up to its
// last-chunk. The body was validated while it was received: only its framing is read again.
void CollectChunks(std::span<const char> input, std::size_t pos, vector<std::string_view>& chunks) {
  const char* pInputBeg = input.data();
  const char* pLast = pInputBeg + input.size();
  while (true) {
    const http::ChunkSizeLine line =
        http::ParseChunkSizeLine(pInputBeg + pos, pLast, http::kNoChunkSizeLineLimit, http::kNoChunkSizeLineLimit,
                                 http::ChunkSizeSyntax::Strict);
    assert(line.state == http::ChunkSizeLine::State::Complete);
    if (line.chunkSize == 0) {
      return;
    }
    pos += line.size;
    chunks.emplace_back(pInputBeg + pos, line.chunkSize);
    pos += line.chunkSize + http::CRLF.size();
  }
}

}  // namespace

Http1Pipeline::BodyDecodeStatus Http1Pipeline::decodeBodyIfReady(ConnectionIt cnxIt, std::span<char> input,
                                                                 bool isChunked, std::size_t maxBodyBytes,
                                                                 std::size_t& consumedBytes, bool deferBody) {
  consumedBytes = 0;
  if (isChunked) {
    return decodeChunkedBody(cnxIt, input, maxBodyBytes, consumedBytes, deferBody);
  }
  // For fixed-length, non-chunked HTTP/1.1 requests there are no trailers per RFC 7230 §4.1.2
  return decodeFixedLengthBody(cnxIt, input, maxBodyBytes, consumedBytes, deferBody);
}

AERONET_ALWAYS_INLINE Http1Pipeline::BodyDecodeStatus Http1Pipeline::decodeFixedLengthBody(ConnectionIt cnxIt,
                                                                                           std::span<const char> input,
                                                                                           std::size_t maxBodyBytes,
                                                                                           std::size_t& consumedBytes,
                                                                                           bool deferBody) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  // Fixed-length (non-chunked) requests never carry trailers (RFC 7230 section 4.1.2), and the trailers of a previous
  // chunked request on this (keep-alive) connection were forgotten when it ended.
  assert(state.pendingRequest.trailerLen == 0);

  const bool hasContentLength = IsSvSizeOnlyView(request._body);
  const std::size_t headerEnd = request.headSpanSize();
  if (!hasContentLength) {
    // No Content-Length and not chunked: treat as no body (common for GET/HEAD). Ready immediately.
    consumedBytes = headerEnd;
    return BodyDecodeStatus::Ready;
  }
  // Note: in HTTP/1.1, trailers are not allowed for non-chunked bodies.
  const std::size_t declaredContentLen = request._body.size();
  // A body exceeding its limits is refused from its head, before it is received: for a body to decompress, the limit of
  // its compressed size too.
  if (maxBodyBytes < declaredContentLen || (_pCtx->config.decompression.maxCompressedBytes < declaredContentLen &&
                                            HttpCodec::WillDecompress(_pCtx->config.decompression, request.headers())))
      [[unlikely]] {
    emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, {});
    return BodyDecodeStatus::Error;
  }
  // Compare against the bytes following the head (headerEnd <= input.size()) rather than computing
  // headerEnd + declaredContentLen, which could wrap around for a huge Content-Length if maxBodyBytes allows it.
  if (input.size() - headerEnd < declaredContentLen) {
    return BodyDecodeStatus::NeedMore;  // need more bytes
  }
  if (!deferBody) {
    request._body = {input.data() + headerEnd, declaredContentLen};
  }
  consumedBytes = headerEnd + declaredContentLen;
  return BodyDecodeStatus::Ready;
}

Http1Pipeline::BodyDecodeStatus Http1Pipeline::decodeChunkedBody(ConnectionIt cnxIt, std::span<char> input,
                                                                 std::size_t maxBodyBytes, std::size_t& consumedBytes,
                                                                 bool deferBody) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  ConnectionState::ChunkedBodyProgress& progress = state.pendingRequest.chunkedBody;
  RawChars& bodyAndTrailers = state.bodyAndTrailersBuffer;

  // Check if we should use direct decompression (avoids copying compressed chunks to bodyAndTrailers)
  const bool decompressFromChunks = HttpCodec::WillDecompress(_pCtx->config.decompression, request.headers());

  // The chunks decoded by the previous reads of this body are not decoded again: this one continues where the last one
  // stopped, at the size line of the first chunk not complete.
  const bool resumed = progress.pos != 0;
  std::size_t pos = progress.pos;
  std::size_t bodySize = progress.bodySize;
  if (!resumed) {
    // The trailers are decoded with the end of the body.
    assert(state.pendingRequest.trailerLen == 0);
    pos = request.headSpanSize();
    bodyAndTrailers.clear();
  }

  // For direct decompression, we collect chunk positions instead of copying data
  vector<std::string_view>& compressedChunks = _pCtx->sharedBuffers.sv;
  compressedChunks.clear();

  char* const pInputBeg = input.data();
  char* const pLast = pInputBeg + input.size();
  const std::size_t maxLineSize = _pCtx->config.maxHeaderBytes;
  while (true) {
    const http::ChunkSizeLine line =
        http::ParseChunkSizeLine(pInputBeg + pos, pLast, maxLineSize, maxBodyBytes, http::ChunkSizeSyntax::Strict);
    if (line.state == http::ChunkSizeLine::State::Incomplete) {
      return BodyDecodeStatus::NeedMore;
    }
    if (line.state == http::ChunkSizeLine::State::Rejected) {
      emitSimpleError(cnxIt, line.status, line.pMessage);
      return BodyDecodeStatus::Error;
    }
    const std::size_t chunkSize = line.chunkSize;
    const std::size_t dataPos = pos + line.size;

    // A chunk making the body exceed its limits is rejected as soon as it is announced, instead of buffering it first.
    if (ExceedsLimit(bodySize, chunkSize, maxBodyBytes) ||
        (decompressFromChunks && ExceedsLimit(bodySize, chunkSize, _pCtx->config.decompression.maxCompressedBytes))) {
      emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge);
      return BodyDecodeStatus::Error;
    }

    // First, check if we have at least the immediate terminating CRLF (written so that it cannot overflow)
    const std::size_t availableBytes = input.size() - dataPos;
    if (availableBytes < http::CRLF.size() || availableBytes - http::CRLF.size() < chunkSize) {
      // A single byte follows the data (or the last-chunk, followed by the trailers or an empty line): a bare LF, as
      // the CR of a CRLF would be.
      if (chunkSize < availableBytes && pInputBeg[dataPos + chunkSize] == '\n') {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Malformed chunk CRLF");
        return BodyDecodeStatus::Error;
      }
      return BodyDecodeStatus::NeedMore;
    }

    if (chunkSize == 0) {
      // Zero-chunk detected. Now parse optional trailer headers (RFC 7230 §4.1.2).
      pos = dataPos;
      const BodyDecodeStatus trailersStatus = decodeTrailers(cnxIt, input, pos);
      if (trailersStatus != BodyDecodeStatus::Ready) {
        return trailersStatus;
      }
      break;
    }

    if (decompressFromChunks) {
      // Just record chunk position for later direct decompression (the chunks of the previous reads are recorded when
      // the body is complete, see below)
      if (!resumed) {
        compressedChunks.emplace_back(pInputBeg + dataPos, chunkSize);
      }
    } else {
      // Append chunk data to body buffer (original path)
      bodyAndTrailers.append(pInputBeg + dataPos, chunkSize);
    }
    bodySize += chunkSize;
    pos = dataPos + chunkSize;
    if (std::memcmp(pInputBeg + pos, http::CRLF.data(), http::CRLF.size()) != 0) {
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Malformed chunk CRLF");
      return BodyDecodeStatus::Error;
    }
    pos += http::CRLF.size();
    progress = {.pos = pos, .bodySize = bodySize};
  }

  consumedBytes = pos;
  if (deferBody) {
    return BodyDecodeStatus::Ready;
  }
  if (decompressFromChunks && bodySize != 0 && resumed) {
    compressedChunks.clear();
    CollectChunks(input, request.headSpanSize(), compressedChunks);
  }
  return finishChunkedBody(cnxIt, compressedChunks, bodySize);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
bool Http1Pipeline::finishDeferredChunkedBody(ConnectionIt cnxIt) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  const HttpRequestView& request = state.request;
  const std::size_t compressedSize = state.pendingRequest.chunkedBody.bodySize;
  vector<std::string_view>& compressedChunks = _pCtx->sharedBuffers.sv;
  compressedChunks.clear();
  if (compressedSize != 0 && HttpCodec::WillDecompress(_pCtx->config.decompression, request.headers())) {
    // The request starts the input buffer, where its body is received.
    CollectChunks(state.inBuffer, request.headSpanSize(), compressedChunks);
  }
  return finishChunkedBody(cnxIt, compressedChunks, compressedSize) == BodyDecodeStatus::Ready;
}
#endif

Http1Pipeline::BodyDecodeStatus Http1Pipeline::finishChunkedBody(ConnectionIt cnxIt,
                                                                 std::span<const std::string_view> compressedChunks,
                                                                 std::size_t compressedSize) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  RawChars& bodyAndTrailers = state.bodyAndTrailersBuffer;

  if (!compressedChunks.empty()) {
    // Perform direct decompression from the input chunks to bodyAndTrailersBuffer
    // Save trailers if present (they were appended to bodyAndTrailers with trailerLen = 0)
    // In direct decompression mode, bodyAndTrailers only contains trailers (no body chunks were copied)
    _pCtx->sharedBuffers.trailersOrTraceEcho.assign(bodyAndTrailers);

    const auto res =
        HttpCodec::DecompressChunkedBody(_pCtx->decompressionState, _pCtx->config.decompression, request,
                                         compressedChunks, compressedSize, bodyAndTrailers, _pCtx->sharedBuffers.buf);
    if (res.message != nullptr) {
      emitSimpleError(cnxIt, res.status, res.message);
      return BodyDecodeStatus::Error;
    }

    // Restore trailers if present
    // Capacity have been reserved in DecompressChunkedBody, so unchecked append is safe here.
    // In addition, a realloc here would mean that we should re-compute the request._body that is set in
    // DecompressChunkedBody.
    assert(bodyAndTrailers.capacity() >= bodyAndTrailers.size() + _pCtx->sharedBuffers.trailersOrTraceEcho.size());
    bodyAndTrailers.unchecked_append(_pCtx->sharedBuffers.trailersOrTraceEcho);

    // Body is set by DecompressChunkedBodyDirect, trailers are appended after
  } else {
    // Body is everything before trailers
    std::size_t bodyLen = bodyAndTrailers.size() - state.pendingRequest.trailerLen;
    request._body = {bodyAndTrailers.data(), bodyLen};
  }

  // The trailers are parsed here, once they are at their final place: the decompression above moves them, and merging
  // their duplicates in place leaves bytes behind them that must not be parsed again.
  assert(request._trailers.empty());  // emptied with the head of the request
  char* const pTrailersEnd = bodyAndTrailers.end();
  char* pTrailerLine = pTrailersEnd - state.pendingRequest.trailerLen;
  while (pTrailerLine < pTrailersEnd) {
    char* pLineEnd = SearchCRLF(pTrailerLine, pTrailersEnd);
    assert(pLineEnd != pTrailersEnd && pLineEnd[1] == '\n');  // guaranteed by the validation of decodeTrailers

    const auto [name, value] = http::ParseHeaderLineTrimValue(pTrailerLine, pLineEnd);
    auto [it, inserted] = request._trailers.emplace(name, value);

    // Same in-place merge as for the headers of the request.
    if (!inserted &&
        !http::MergeHeaderInPlace(request._trailers, it, value, _pCtx->sharedBuffers.buf, bodyAndTrailers.data(),
                                  pTrailerLine, _pCtx->config.mergeUnknownRequestHeaders)) {
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Invalid trailer headers");
      return BodyDecodeStatus::Error;
    }

    pTrailerLine = pLineEnd + http::CRLF.size();
  }

  return BodyDecodeStatus::Ready;
}

Http1Pipeline::BodyDecodeStatus Http1Pipeline::decodeTrailers(ConnectionIt cnxIt, std::span<char> input,
                                                              std::size_t& pos) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  RawChars& bodyAndTrailers = state.bodyAndTrailersBuffer;
  char* const pInputBeg = input.data();
  char* const pLast = pInputBeg + input.size();

  // Trailers are terminated by a blank line (CRLF). Check if trailers are present (not immediate CRLF).
  char* pTrailerStart = pInputBeg + pos;
  if (std::memcmp(pTrailerStart, http::CRLF.data(), http::CRLF.size()) == 0) {
    // No trailers, just the terminating CRLF
    pos += http::CRLF.size();
    return BodyDecodeStatus::Ready;
  }

  std::size_t trailerEndPos;  // Track end position for later

  // Validate trailers and find the end position
  std::size_t tempPos = pos;
  const std::size_t maxTrailerSize = _pCtx->config.maxHeaderBytes;
  while (true) {
    // The trailer lines count toward maxHeaderBytes with their CRLF, the final empty line does not. Only the bytes
    // before the current line are counted: the input may hold the requests pipelined behind this one.
    char* const pLineStart = pInputBeg + tempPos;
    if (pLineStart > pTrailerStart + maxTrailerSize) {
      emitSimpleError(cnxIt, http::StatusCodeRequestHeaderFieldsTooLarge);
      return BodyDecodeStatus::Error;
    }

    const auto lineEndIt = SearchCRLF(pLineStart, pLast);
    if (lineEndIt == pLast || lineEndIt[1] != '\n') {
      // Not complete yet, or with a bare CR: waited for only if more input can still make it valid.
      if (!CanStillBeValidLines(pLineStart, pLast)) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Malformed trailer line");
        return BodyDecodeStatus::Error;
      }
      // The trailers are bounded whether their line ends are received or not: a client could otherwise make the server
      // buffer (and parse again at each read) a trailer line without end, without limit. The received bytes of the
      // current line count, unless they are the CR of the final empty line: the limit does not depend on the reads.
      const bool finalLineStarted = pLast - pLineStart == 1 && *pLineStart == '\r';
      if (!finalLineStarted && pLast > pTrailerStart + maxTrailerSize) {
        emitSimpleError(cnxIt, http::StatusCodeRequestHeaderFieldsTooLarge);
        return BodyDecodeStatus::Error;
      }
      return BodyDecodeStatus::NeedMore;
    }

    char* pLineLast = lineEndIt;

    // Detect blank line (end of trailers)
    if (pLineStart == pLineLast) {
      trailerEndPos = static_cast<std::size_t>(lineEndIt - pInputBeg) + http::CRLF.size();
      break;
    }

    // Parse trailer field: name:value
    const auto [nameView, valueView] = http::ParseHeaderLineTrimValue(pLineStart, pLineLast);
    if (!http::IsValidHeaderName(nameView) || !http::IsValidReceivedHeaderValue(valueView)) {
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Malformed / invalid trailer header");
      return BodyDecodeStatus::Error;
    }

    // Lower-case trailer name in place for case-sensitive lookup later
    tolower(pLineStart, nameView.size());

    // Check forbidden headers
    if (http::IsForbiddenTrailerHeader(LowerAsciiKey{nameView})) {
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Forbidden trailer header");
      return BodyDecodeStatus::Error;
    }

    tempPos = static_cast<std::size_t>(lineEndIt - pInputBeg) + http::CRLF.size();
  }

  // Copy all trailer data at once, behind the body (parsed by decodeChunkedBody, once the body is decoded)
  const std::size_t trailerLen = trailerEndPos - pos - http::CRLF.size();  // Exclude final CRLF
  bodyAndTrailers.append(pTrailerStart, trailerLen);
  state.pendingRequest.trailerLen = SafeCast<uint32_t>(trailerLen);

  pos = trailerEndPos;
  return BodyDecodeStatus::Ready;
}

}  // namespace aeronet::internal
