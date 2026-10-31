#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/header-line-parse.hpp"
#include "aeronet/header-merge.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/http1-pipeline.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/reserved-headers.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/search-crlf.hpp"
#include "aeronet/sv-to-sv-map.hpp"
#include "aeronet/tolower-str.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

namespace {

// Tells whether adding 'size' bytes to 'current' ones exceeds 'limit' (written so that it cannot overflow).
constexpr bool ExceedsLimit(std::size_t current, std::size_t size, std::size_t limit) noexcept {
  return size > limit || current > limit - size;
}

// A chunk-size line (RFC 9112 §7.1: chunk-size [ chunk-ext ] CRLF), as parsed by ParseChunkSizeLine().
struct ChunkSizeLine {
  enum class State : uint8_t { Complete, Incomplete, Rejected };

  void setRejected(http::StatusCode statusCode, const char* pRejectedMessage) {
    status = statusCode;
    pMessage = pRejectedMessage;
    state = State::Rejected;
  }

  std::size_t chunkSize{0};        // Complete only
  std::size_t size{0};             // bytes of the line, CRLF included (Complete only)
  const char* pMessage = nullptr;  // Rejected only: the body of the error response
  http::StatusCode status{};       // Rejected only: the status of the error response
  State state{State::Complete};
};

// Parses the size line of the chunk starting at pFirst. The line is bounded by maxLineSize, CRLF included, whether it
// is received at once or not: without its end, it takes at least one more byte. Otherwise, a client could make the
// server buffer (and search again at each read) an unterminated line without limit. Chunk sizes above maxChunkSize are
// rejected.
ChunkSizeLine ParseChunkSizeLine(const char* pFirst, const char* pLast, std::size_t maxLineSize,
                                 std::size_t maxChunkSize) noexcept {
  const char* pLineEnd = SearchCRLF(pFirst, pLast);
  const bool isComplete = pLineEnd != pLast;
  const std::size_t lineSize = static_cast<std::size_t>(pLineEnd - pFirst) + (isComplete ? http::CRLF.size() : 1U);

  ChunkSizeLine out;

  if (lineSize > maxLineSize) {
    out.setRejected(http::StatusCodeBadRequest, "Chunk size line too long");
    return out;
  }

  if (!isComplete) {
    out.state = ChunkSizeLine::State::Incomplete;
    return out;
  }

  if (pLineEnd[1] != '\n') {
    out.setRejected(http::StatusCodeBadRequest, "Malformed chunk size line");
    return out;
  }

  // chunk-size = 1*HEXDIG (RFC 9112 §7.1). An empty chunk-size is not a last-chunk.
  const char* pSizeEnd = std::find(pFirst, pLineEnd, ';');
  if (pSizeEnd == pFirst) {
    out.setRejected(http::StatusCodeBadRequest, "Invalid chunk size");
    return out;
  }
  // Chunk extensions are ignored, but must not contain control characters: a parser accepting a bare LF as chunk
  // line terminator would otherwise not agree with us on where the chunk data starts. Bytes above 0x7F are allowed, as
  // in a quoted-string (RFC 9112 §7.1.1).
  if (!http::IsValidReceivedHeaderValue(std::string_view(pSizeEnd, pLineEnd))) {
    out.setRejected(http::StatusCodeBadRequest, "Invalid chunk extension");
    return out;
  }
  for (const char* pDigit = pFirst; pDigit != pSizeEnd; ++pDigit) {
    const int8_t digit = FromHexDigit(*pDigit);
    if (digit < 0) {
      out.setRejected(http::StatusCodeBadRequest, "Invalid chunk size");
      return out;
    }
    // Never let the chunk size wrap around (it could become 0, ending the body early) - can only happen if
    // maxChunkSize does not already bound it.
    if (out.chunkSize > (std::numeric_limits<std::size_t>::max() >> 4U)) {
      out.setRejected(http::StatusCodePayloadTooLarge, "Chunk size too large");
      return out;
    }
    out.chunkSize = (out.chunkSize << 4U) | static_cast<std::size_t>(digit);
    if (maxChunkSize < out.chunkSize) {
      out.setRejected(http::StatusCodePayloadTooLarge, "Chunk size too large");
      return out;
    }
  }
  out.size = lineSize;
  return out;
}

// Collects in 'chunks' the data of the chunks of the body in 'input' starting at the offset 'pos', up to its
// last-chunk. The body was validated while it was received: only its framing is read again.
void CollectChunks(std::span<const char> input, std::size_t pos, vector<std::string_view>& chunks) {
  static constexpr std::size_t kNoLimit = std::numeric_limits<std::size_t>::max();

  const char* pInputBeg = input.data();
  const char* pLast = pInputBeg + input.size();
  while (true) {
    const ChunkSizeLine line = ParseChunkSizeLine(pInputBeg + pos, pLast, kNoLimit, kNoLimit);
    assert(line.state == ChunkSizeLine::State::Complete);
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
                                                                 bool isChunked, bool expectContinue,
                                                                 std::size_t maxBodyBytes, std::size_t& consumedBytes) {
  consumedBytes = 0;
  if (isChunked) {
    return decodeChunkedBody(cnxIt, input, expectContinue, maxBodyBytes, consumedBytes);
  }
  // For fixed-length, non-chunked HTTP/1.1 requests there are no trailers per RFC 7230 §4.1.2
  return decodeFixedLengthBody(cnxIt, input, expectContinue, maxBodyBytes, consumedBytes);
}

Http1Pipeline::BodyDecodeStatus Http1Pipeline::decodeFixedLengthBody(ConnectionIt cnxIt, std::span<const char> input,
                                                                     bool expectContinue, std::size_t maxBodyBytes,
                                                                     std::size_t& consumedBytes) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  // Fixed-length (non-chunked) requests never carry trailers (RFC 7230 section 4.1.2). Clear any trailerLen
  // left over from a previous chunked request on this (keep-alive) connection so the request-finalization
  // path does not spuriously re-parse stale trailer bytes into this request's trailers.
  state.trailerLen = 0;
  auto optContentLength = request.headerValue(http::ContentLength);
  const std::size_t headerEnd = request.headSpanSize();
  if (!optContentLength) {
    // No Content-Length and not chunked: treat as no body (common for GET/HEAD). Ready immediately.
    request._body = std::string_view{};
    consumedBytes = headerEnd;
    return BodyDecodeStatus::Ready;
  }
  std::string_view lenViewAll = *optContentLength;
  // Note: in HTTP/1.1, trailers are not allowed for non-chunked bodies.
  std::size_t declaredContentLen = 0;
  auto [ptr, err] = std::from_chars(lenViewAll.data(), lenViewAll.data() + lenViewAll.size(), declaredContentLen);
  if (err != std::errc() || ptr != lenViewAll.data() + lenViewAll.size()) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Invalid Content-Length");
    return BodyDecodeStatus::Error;
  }
  if (maxBodyBytes < declaredContentLen) {
    emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, {});
    return BodyDecodeStatus::Error;
  }
  if (expectContinue && declaredContentLen > 0) {
    _pCtx->io.queueData(cnxIt, HttpMessageData(RawChars{http::HTTP11_100_CONTINUE}));
  }
  // Compare against the bytes following the head (headerEnd <= input.size()) rather than computing
  // headerEnd + declaredContentLen, which could wrap around for a huge Content-Length if maxBodyBytes allows it.
  if (input.size() - headerEnd < declaredContentLen) {
    return BodyDecodeStatus::NeedMore;  // need more bytes
  }
  request._body = {input.data() + headerEnd, declaredContentLen};
  consumedBytes = headerEnd + declaredContentLen;
  return BodyDecodeStatus::Ready;
}

Http1Pipeline::BodyDecodeStatus Http1Pipeline::decodeChunkedBody(ConnectionIt cnxIt, std::span<char> input,
                                                                 bool expectContinue, std::size_t maxBodyBytes,
                                                                 std::size_t& consumedBytes) {
  if (expectContinue) {
    _pCtx->io.queueData(cnxIt, HttpMessageData(RawChars{http::HTTP11_100_CONTINUE}));
  }
  const BodyDecodeStatus status = decodeNextChunks(cnxIt, input, maxBodyBytes, consumedBytes);
  if (status != BodyDecodeStatus::NeedMore) {
    // Complete, or rejected: the next body starts from its first chunk.
    _pCtx->connections.connectionState(cnxIt).chunkedBody = {};
  }
  return status;
}

Http1Pipeline::BodyDecodeStatus Http1Pipeline::decodeNextChunks(ConnectionIt cnxIt, std::span<char> input,
                                                                std::size_t maxBodyBytes, std::size_t& consumedBytes) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  ConnectionState::ChunkedBodyProgress& progress = state.chunkedBody;
  RawChars& bodyAndTrailers = state.bodyAndTrailersBuffer;

  // Check if we should use direct decompression (avoids copying compressed chunks to bodyAndTrailers)
  const http::StatusCode decompressCode = HttpCodec::WillDecompress(_pCtx->config.decompression, request.headers());
  if (decompressCode == http::StatusCodeBadRequest) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Malformed Content-Encoding");
    return BodyDecodeStatus::Error;
  }
  const bool decompressFromChunks = decompressCode == http::StatusCodeOK;

  // The chunks decoded by the previous reads of this body are not decoded again: this one continues where the last one
  // stopped, at the size line of the first chunk not complete.
  const bool resumed = progress.pos != 0;
  std::size_t pos = progress.pos;
  std::size_t bodySize = progress.bodySize;
  if (!resumed) {
    pos = request.headSpanSize();
    bodyAndTrailers.clear();
    state.trailerLen = 0;
  }

  // For direct decompression, we collect chunk positions instead of copying data
  vector<std::string_view>& compressedChunks = _pCtx->sharedBuffers.sv;
  compressedChunks.clear();

  char* const pInputBeg = input.data();
  char* const pLast = pInputBeg + input.size();
  const std::size_t maxLineSize = _pCtx->config.maxHeaderBytes;
  while (true) {
    const ChunkSizeLine line = ParseChunkSizeLine(pInputBeg + pos, pLast, maxLineSize, maxBodyBytes);
    if (line.state == ChunkSizeLine::State::Incomplete) {
      return BodyDecodeStatus::NeedMore;
    }
    if (line.state == ChunkSizeLine::State::Rejected) {
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

  if (decompressFromChunks && bodySize != 0) {
    if (resumed) {
      compressedChunks.clear();
      CollectChunks(input, request.headSpanSize(), compressedChunks);
    }
    // Perform direct decompression from the input chunks to bodyAndTrailersBuffer
    // Save trailers if present (they were appended to bodyAndTrailers with trailerLen = 0)
    // In direct decompression mode, bodyAndTrailers only contains trailers (no body chunks were copied)
    _pCtx->sharedBuffers.trailersOrTraceEcho.assign(bodyAndTrailers);

    const auto res =
        HttpCodec::DecompressChunkedBody(_pCtx->decompressionState, _pCtx->config.decompression, request,
                                         compressedChunks, bodySize, bodyAndTrailers, _pCtx->sharedBuffers.buf);
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
    std::size_t bodyLen = bodyAndTrailers.size() - state.trailerLen;
    request._body = {bodyAndTrailers.data(), bodyLen};
  }

  consumedBytes = pos;
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

  // Parse trailer headers - copy raw trailer data to bodyAndTrailers buffer
  std::size_t trailerEndPos;  // Track end position for later

  // First pass: validate trailers and find the end position
  std::size_t tempPos = pos;
  while (true) {
    const auto lineEndIt = SearchCRLF(pInputBeg + tempPos, pLast);
    if (lineEndIt == pLast) {
      // The trailers are bounded whether their line ends are received or not: a client could otherwise make the server
      // buffer (and parse again at each read) a trailer line without end, without limit.
      const std::size_t receivedTrailerSize = static_cast<std::size_t>(pLast - pTrailerStart);
      if (receivedTrailerSize > _pCtx->config.maxHeaderBytes) {
        emitSimpleError(cnxIt, http::StatusCodeRequestHeaderFieldsTooLarge);
        return BodyDecodeStatus::Error;
      }
      return BodyDecodeStatus::NeedMore;
    }
    if (lineEndIt[1] != '\n') {
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Malformed trailer line");
      return BodyDecodeStatus::Error;
    }
    // Check total trailer size limit
    const std::size_t trailerSize = static_cast<std::size_t>((pInputBeg + tempPos) - pTrailerStart);
    if (trailerSize > _pCtx->config.maxHeaderBytes) {
      emitSimpleError(cnxIt, http::StatusCodeRequestHeaderFieldsTooLarge);
      return BodyDecodeStatus::Error;
    }

    char* pLineStart = pInputBeg + tempPos;
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

  // Copy all trailer data at once to avoid reallocation during parsing
  const std::size_t trailerLen = trailerEndPos - pos - http::CRLF.size();  // Exclude final CRLF
  bodyAndTrailers.append(pTrailerStart, trailerLen);
  state.trailerLen = SafeCast<uint32_t>(trailerLen);

  // Second pass: parse trailers from copied data in bodyAndTrailers
  char* pTrailerDataEnd = bodyAndTrailers.data() + bodyAndTrailers.size();
  char* pTrailerDataBeg = pTrailerDataEnd - trailerLen;

  if (!parseHeadersUnchecked(state.request._trailers, bodyAndTrailers.data(), pTrailerDataBeg, pTrailerDataEnd)) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Invalid trailer headers");
    return BodyDecodeStatus::Error;
  }

  pos = trailerEndPos;
  return BodyDecodeStatus::Ready;
}

bool Http1Pipeline::parseHeadersUnchecked(SvToSvMap& headersMap, char* bufferBeg, char* first, char* last) {
  headersMap.clear();
  while (first < last) {
    // Find line end
    char* lineEnd = SearchCRLF(first, last);
    assert(lineEnd != last && lineEnd[1] == '\n');  // guaranteed by the validating pass that built this buffer

    // No check is made on header line format here
    auto [headerName, headerValue] = http::ParseHeaderLineTrimValue(first, lineEnd);

    auto [it, inserted] = headersMap.emplace(headerName, headerValue);

    // Store trailer using the in-place merge helper so semantics/pointer updates match request parsing.
    if (!inserted && !http::MergeHeaderInPlace(headersMap, it, headerValue, _pCtx->sharedBuffers.buf, bufferBeg, first,
                                               _pCtx->config.mergeUnknownRequestHeaders)) {
      return false;
    }

    first = lineEnd + http::CRLF.size();
  }
  return true;
}

}  // namespace aeronet::internal
