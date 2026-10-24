#include "http1-response-parser.hpp"

#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/internal/http-response-access.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/search-crlf.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/tolower-str.hpp"

namespace aeronet {

namespace {

// True if the last token of a (possibly comma-separated) Transfer-Encoding value is "chunked".
constexpr bool LastTransferEncodingIsChunked(std::string_view value) {
  const auto comma = value.rfind(',');
  const std::string_view last = comma == std::string_view::npos ? value : value.substr(comma + 1);
  return CaseInsensitiveEqual(TrimOws(last), http::chunked);
}

enum class ChunkLineStatus : uint8_t { Complete, NeedMore, Error };

struct ChunkLineResult {
  std::size_t chunkSize;
  std::size_t nextPos;
  ChunkLineStatus status;
};

// Convert one ASCII hexadecimal digit without a lookup table. Decimal digits are the common path for
// HTTP chunk sizes; folding alphabetic digits to lowercase keeps the uncommon branch compact.
constexpr uint32_t HexDigitValue(unsigned char ch) noexcept {
  const uint32_t decimal = ch - static_cast<unsigned char>('0');
  if (decimal <= 9U) {
    return decimal;
  }
  const uint32_t alpha = (ch | 0x20U) - static_cast<unsigned char>('a');
  return alpha <= 5U ? alpha + 10U : 16U;
}

// Parse a complete chunk-size line in one forward pass. The common path touches each hexadecimal byte once,
// folds overflow checking into the shift, and consumes CRLF directly. Extensions remain opaque as required
// by HTTP chunk decoding, but their bytes and line ending are validated while they are skipped.
constexpr ChunkLineResult ParseChunkSizeLine(std::string_view buffer, std::size_t pos) noexcept {
  const char* const base = buffer.data();
  const char* cur = base + pos;
  const char* const end = base + buffer.size();
  std::size_t chunkSize = 0;
  bool hasDigit = false;
  bool trailingOws = false;

  const auto finish = [&chunkSize, base, &hasDigit](const char* next) {
    return ChunkLineResult{
        .chunkSize = chunkSize,
        .nextPos = static_cast<std::size_t>(next - base),
        .status = hasDigit ? ChunkLineStatus::Complete : ChunkLineStatus::Error,
    };
  };

  while (cur != end) {
    const unsigned char ch = static_cast<unsigned char>(*cur++);
    const uint32_t digit = HexDigitValue(ch);
    if (digit < 16U) {
      if (trailingOws || chunkSize > (std::numeric_limits<std::size_t>::max() >> 4U)) {
        return {.chunkSize = 0, .nextPos = pos, .status = ChunkLineStatus::Error};
      }
      chunkSize = (chunkSize << 4U) | digit;
      hasDigit = true;
      continue;
    }

    if (ch == '\r') {
      if (cur == end) {
        return {.chunkSize = 0, .nextPos = pos, .status = ChunkLineStatus::NeedMore};
      }
      if (*cur != '\n') {
        return {.chunkSize = 0, .nextPos = pos, .status = ChunkLineStatus::Error};
      }
      return finish(cur + 1);
    }
    if (ch == ';') {
      if (!hasDigit) {
        return {.chunkSize = 0, .nextPos = pos, .status = ChunkLineStatus::Error};
      }
      while (cur != end) {
        const unsigned char extensionCh = static_cast<unsigned char>(*cur++);
        if (extensionCh == '\r') {
          if (cur == end) {
            return {.chunkSize = 0, .nextPos = pos, .status = ChunkLineStatus::NeedMore};
          }
          if (*cur != '\n') {
            return {.chunkSize = 0, .nextPos = pos, .status = ChunkLineStatus::Error};
          }
          return finish(cur + 1);
        }
        if ((extensionCh < 0x20U && extensionCh != '\t') || extensionCh == 0x7FU) {
          return {.chunkSize = 0, .nextPos = pos, .status = ChunkLineStatus::Error};
        }
      }
      return {.chunkSize = 0, .nextPos = pos, .status = ChunkLineStatus::NeedMore};
    }
    if (ch == ' ' || ch == '\t') {
      trailingOws = hasDigit;
      continue;
    }
    return {.chunkSize = 0, .nextPos = pos, .status = ChunkLineStatus::Error};
  }
  return {.chunkSize = 0, .nextPos = pos, .status = ChunkLineStatus::NeedMore};
}

// Scan a (possibly comma-separated) Connection token list, flagging the close / keep-alive directives.
// Connection is defined as a list of options (e.g. "close, foo" / "keep-alive, Upgrade"), so each token
// must be matched individually rather than comparing the whole value. Flags are only ever set, never
// cleared, so directives accumulate across repeated Connection header lines.
constexpr void ScanConnectionTokens(std::string_view value, bool& closeSeen, bool& keepAliveSeen) {
  while (!value.empty()) {
    const auto comma = value.find(',');
    const std::string_view token = TrimOws(value.substr(0, comma));
    if (CaseInsensitiveEqual(token, http::close)) {
      closeSeen = true;
    } else if (CaseInsensitiveEqual(token, http::keepalive)) {
      keepAliveSeen = true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    value.remove_prefix(comma + 1);
  }
}

}  // namespace

void Http1ResponseParser::InstallBodyStorage(HttpResponse& resp, std::string_view body, std::string_view contentType,
                                             RawChars* owner) {
  if (owner == nullptr || body.empty()) {
    resp.body(body, contentType);
    return;
  }

  assert(body.data() >= owner->data());
  const std::size_t bodyOffset = static_cast<std::size_t>(body.data() - owner->data());
  assert(bodyOffset <= owner->size());
  assert(body.size() == owner->size() - bodyOffset);
  if (owner->capacity() - body.size() > body.size()) {
    resp.body(body, contentType);
    owner->clear();
    return;
  }

  RawChars replacement(owner->capacity());
  resp.setBodyHeaders(contentType, body.size(), HttpMessage::BodySetContext::Captured);
  resp.setBodyInternal({});
  if (bodyOffset == 0) {
    resp.setCapturedPayload(std::move(*owner));
  } else {
    resp.setCapturedPayload(std::move(*owner), bodyOffset);
  }
  *owner = std::move(replacement);
}

void Http1ResponseParser::reset(bool headRequest, bool streamHead) noexcept {
  _bodyBuf->clear();
  _pos = 0;
  _bodyStart = 0;
  _contentTypeOff = 0;
  _contentTypeLen = 0;
  _bodyRemaining = 0;
  _receiveBuf = nullptr;
  _statusCode = 0;
  _framing = Framing::None;
  _state = State::StatusLine;
  _headRequest = headRequest;
  _streamHead = streamHead;
  _streaming = false;
  _gatheredPiece = false;
  _bufferedBody = {};
  _keepAlive = false;
  _http11 = false;
  _connectionCloseSeen = false;
  _connectionKeepAliveSeen = false;
  _hasContentLength = false;
  _chunked = false;
}

Http1ResponseParser::Status Http1ResponseParser::installBody(HttpResponse& resp, std::string_view buffer) {
  if (_framing == Framing::None) {
    return Status::Complete;  // bodyless response (HEAD / 204 / 304 / 1xx)
  }
  // Chunked bodies are reassembled in _bodyBuf; Length / UntilClose bodies are contiguous suffixes of the
  // receive buffer. Either allocation can become the response's owner when it is suitably sized.
  const std::string_view body =
      _framing == Framing::Chunked ? std::string_view(*_bodyBuf) : buffer.substr(_bodyStart, _pos - _bodyStart);

  // Automatic decompression: decode straight from `body` (receive buffer / de-framed chunks) into the
  // borrowed scratch buffer and drop the now-stale Content-Encoding header (the body is not installed yet, so
  // removal is cheap and legal). No intermediate copy of the compressed body.
  std::string_view decoded = body;
  if (_decode.state != nullptr) {
    const std::string_view contentEncoding = resp.headerValueOrEmpty(http::ContentEncoding);
    if (!contentEncoding.empty()) {
      // An empty body has nothing to decode: it is delivered like a decoded one, without its coding.
      if (!body.empty()) {
        const auto res = HttpCodec::DecompressFullBody(*_decode.state, *_decode.config, contentEncoding, body,
                                                       *_decode.out, *_decode.tmp, decoded);
        if (res.status != http::StatusCodeOK) {
          return res.status == http::StatusCodePayloadTooLarge ? Status::TooLarge : Status::Error;
        }
      }
      resp.headerRemoveLine(http::ContentEncoding);
    }
  }

  if (_streamHead) {
    // Response to a sink request received whole: it stays in the scratch buffers, from where HttpClient delivers it to
    // the sink once known to be the final response, without copy.
    _bufferedBody = decoded;
    return Status::Complete;
  }

  // Transfer a suitably-sized scratch allocation into the response and preserve an equal-capacity empty replacement.
  // If the scratch is over twice the body size, retain it and copy the small body instead of making both the response
  // and client pin that high-water allocation. The common stable-size path keeps the same allocation count and retained
  // capacity as before, but removes the large final memcpy.
  RawChars* owner = nullptr;
  if (decoded.data() != body.data()) {  // decoded into a scratch buffer (an identity coding returns the body itself)
    owner = WholeBufferOwner(decoded, *_decode.out);
    if (owner == nullptr) {
      owner = WholeBufferOwner(decoded, *_bodyBuf);
    }
  } else if (_framing == Framing::Chunked) {
    owner = _bodyBuf;
  } else if (_receiveBuf != nullptr && buffer.data() == _receiveBuf->data() && buffer.size() == _receiveBuf->size() &&
             _pos == buffer.size()) {
    owner = _receiveBuf;
  }
  const std::string_view contentType =
      _contentTypeLen == 0 ? http::ContentTypeApplicationOctetStream : buffer.substr(_contentTypeOff, _contentTypeLen);
  InstallBodyStorage(resp, decoded, contentType, owner);
  return Status::Complete;
}

Http1ResponseParser::Status Http1ResponseParser::parse(RawChars& buffer, bool eof, HttpResponse& resp,
                                                       std::size_t maxResponseBytes) {
  _receiveBuf = &buffer;
  const Status status = parse(std::span<char>(buffer.data(), buffer.size()), eof, resp, maxResponseBytes);
  _receiveBuf = nullptr;
  return status;
}

Http1ResponseParser::Status Http1ResponseParser::decideFraming() {
  // Connection persistence: HTTP/1.1 defaults to keep-alive, HTTP/1.0 defaults to close.
  bool keepAlive = _http11;
  if (_connectionCloseSeen) {
    keepAlive = false;
  } else if (_connectionKeepAliveSeen) {
    keepAlive = true;
  }

  // The cursor sits on the first body byte (just past the blank line ending the header block).
  _bodyStart = _pos;

  // 1xx interim responses are discarded (and the parser restarted) in parse() before decideFraming() runs,
  // so the status is always a final one (>= 200) here: only 204/304 (and a HEAD request) are bodyless.
  assert(_statusCode >= 200);
  const http::StatusCode code = _statusCode;
  const bool bodyless = _headRequest || code == http::StatusCodeNoContent || code == http::StatusCodeNotModified;

  if (bodyless) {
    _framing = Framing::None;
    _keepAlive = keepAlive;
    _state = State::Done;
    return Status::Complete;
  }

  if (_chunked) {
    _framing = Framing::Chunked;
    _state = State::BodyChunkSize;
    _keepAlive = keepAlive;
    return Status::NeedMore;
  }

  if (_hasContentLength) {
    _framing = Framing::Length;
    _keepAlive = keepAlive;
    _state = _bodyRemaining == 0 ? State::Done : State::BodyLength;
    return _bodyRemaining == 0 ? Status::Complete : Status::NeedMore;
  }

  // No explicit framing: body runs until the server closes the connection.
  _framing = Framing::UntilClose;
  _keepAlive = false;  // cannot reuse a connection whose body end is signalled by close
  _state = State::BodyUntilClose;
  return Status::NeedMore;
}

Http1ResponseParser::Status Http1ResponseParser::parseBody(std::string_view buffer, bool eof,
                                                           std::size_t maxResponseBytes) {
  // A streamed body is delivered piece by piece as it arrives: only the buffered one is bounded by maxResponseBytes.
  switch (_framing) {
    case Framing::Length: {
      const std::size_t available = buffer.size() - _pos;
      const std::size_t take = available < _bodyRemaining ? available : _bodyRemaining;
      if (_streaming) {
        if (take == 0) {
          return eof ? Status::Truncated : Status::NeedMore;
        }
        _bodyRemaining -= take;
        if (_bodyRemaining == 0) {
          _state = State::Done;  // the next parse() call completes
        }
        return bodyPiece(take);
      }
      // The body stays in place in the receive buffer; only the cursor advances (zero-copy until install).
      if ((_pos - _bodyStart) + take > maxResponseBytes) {
        return Status::TooLarge;
      }
      _pos += take;
      _bodyRemaining -= take;
      if (_bodyRemaining == 0) {
        _state = State::Done;
        return Status::Complete;
      }
      return eof ? Status::Truncated : Status::NeedMore;
    }

    case Framing::UntilClose: {
      const std::size_t available = buffer.size() - _pos;
      if (_streaming) {
        if (available != 0) {
          return bodyPiece(available);
        }
      } else {
        if ((_pos - _bodyStart) + available > maxResponseBytes) {
          return Status::TooLarge;
        }
        _pos += available;
      }
      if (eof) {
        _state = State::Done;
        return Status::Complete;
      }
      return Status::NeedMore;
    }

    default: {
      // parse() resolves bodyless / zero-length framings (None) to Complete before any body parsing, so
      // parseBody() is only ever entered for Length / UntilClose / Chunked. None / default is unreachable.
      assert(_framing == Framing::Chunked);
      const Status status = parseChunkedBody(buffer, eof, maxResponseBytes);
      if (_streaming && status != Status::BodyPiece && !_bodyBuf->empty()) {
        // Deliver the small chunks gathered so far before the status ending them, which the next call finds again
        // (it only depends on the received bytes from the current position).
        _gatheredPiece = true;
        return Status::BodyPiece;
      }
      return status;
    }
  }
}

Http1ResponseParser::Status Http1ResponseParser::parseChunkedBody(std::string_view buffer, bool eof,
                                                                  std::size_t maxResponseBytes) {
  // Chunk-size and trailer lines are never part of the body: an endless one must not grow the receive buffer forever.
  const auto incompleteLine = [&buffer, this, eof, maxResponseBytes] {
    if (buffer.size() - _pos > maxResponseBytes) {
      return Status::TooLarge;
    }
    return eof ? EofInLine(buffer.substr(_pos)) : Status::NeedMore;
  };
  for (;;) {
    if (_state == State::BodyChunkSize) {
      const ChunkLineResult chunkLine = ParseChunkSizeLine(buffer, _pos);
      if (chunkLine.status == ChunkLineStatus::Error) {
        return Status::Error;
      }
      if (chunkLine.status == ChunkLineStatus::NeedMore) {
        return incompleteLine();
      }
      _pos = chunkLine.nextPos;
      if (chunkLine.chunkSize == 0) {
        _state = State::BodyChunkTrailers;
      } else {
        _bodyRemaining = chunkLine.chunkSize;
        _state = State::BodyChunkData;
      }
      continue;
    }

    if (_state == State::BodyChunkData) {
      // A chunk being received always has data left (a zero-size chunk is the last-chunk marker).
      assert(_bodyRemaining != 0);
      const std::size_t available = buffer.size() - _pos;
      const std::size_t take = available < _bodyRemaining ? available : _bodyRemaining;
      if (_streaming) {
        if (take == 0) {
          return eof ? Status::Truncated : Status::NeedMore;
        }
        if (take >= kMinDirectPieceBytes && !_bodyBuf->empty()) {
          // The small chunks gathered before this one are delivered first: the next call comes back here.
          _gatheredPiece = true;
          return Status::BodyPiece;
        }
        _bodyRemaining -= take;
        if (_bodyRemaining == 0) {
          _state = State::BodyChunkCrlf;
        }
        if (take >= kMinDirectPieceBytes) {
          return bodyPiece(take);  // delivered from the receive buffer, without copy
        }
        // A small piece is gathered with the next ones, to be delivered with them (see kMinDirectPieceBytes).
        _bodyBuf->append(buffer.data() + _pos, take);
        _pos += take;
        continue;
      }
      const std::size_t bodySize = static_cast<std::size_t>(_bodyBuf->size());
      if (bodySize > maxResponseBytes || take > maxResponseBytes - bodySize) {
        return Status::TooLarge;
      }
      _bodyBuf->append(buffer.data() + _pos, take);
      _pos += take;
      _bodyRemaining -= take;
      if (_bodyRemaining != 0) {
        return eof ? Status::Truncated : Status::NeedMore;
      }
      _state = State::BodyChunkCrlf;
      continue;
    }

    if (_state == State::BodyChunkCrlf) {
      const std::size_t available = buffer.size() - _pos;
      if (available == 0) {
        return eof ? Status::Truncated : Status::NeedMore;
      }
      if (buffer[_pos] != '\r') {
        return Status::Error;
      }
      if (available == 1) {
        return eof ? Status::Truncated : Status::NeedMore;
      }
      if (buffer[_pos + 1] != '\n') {
        return Status::Error;
      }
      _pos += http::CRLF.size();
      _state = State::BodyChunkSize;
      continue;
    }

    // The three sub-states above each return or `continue`, so the only state left here is the trailer
    // block. Consume trailer lines until an empty line. Trailers are not surfaced.
    assert(_state == State::BodyChunkTrailers);
    for (const char *const base = buffer.data(), *end = base + buffer.size();;) {
      const char* const first = base + _pos;
      const char* const lineEnd = SearchCRLF(first, end);
      if (lineEnd == end) {
        return incompleteLine();
      }
      if (lineEnd[1] != '\n') {
        return Status::Error;
      }
      _pos = static_cast<std::size_t>(lineEnd - base) + http::CRLF.size();
      if (lineEnd == first) {
        _state = State::Done;
        return Status::Complete;
      }
    }
  }
}

Http1ResponseParser::Status Http1ResponseParser::parse(std::span<char> buffer, bool eof, HttpResponse& resp,
                                                       std::size_t maxResponseBytes) {
  if (_gatheredPiece) {
    // Delivered by the previous call.
    _bodyBuf->clear();
    _gatheredPiece = false;
  }
  // --- Status line + headers ---
  const char* const bufferBegin = buffer.data();
  const char* const bufferEnd = bufferBegin + buffer.size();
  while (_state == State::StatusLine || _state == State::Headers) {
    const char* const lineBegin = bufferBegin + _pos;
    const char* const lineEnd = SearchCRLF(lineBegin, bufferEnd);
    if (lineEnd == bufferEnd) {
      // No complete line yet. Everything buffered so far is head bytes (the body can only begin after the
      // empty line we have not seen), so bound the buffer itself: otherwise a peer streaming an endless
      // header line keeps `_pos` pinned at the line start and grows the receive buffer without limit.
      if (buffer.size() > maxResponseBytes) {
        return Status::TooLarge;
      }
      return eof ? EofInLine(std::string_view(lineBegin, bufferEnd)) : Status::NeedMore;
    }
    if (lineEnd[1] != '\n') {
      return Status::Error;
    }
    const std::string_view line(lineBegin, lineEnd);
    _pos = static_cast<std::size_t>(lineEnd - bufferBegin) + http::CRLF.size();
    // Every consumed head line (status line, a header, or the terminating empty line) counts against the
    // total response budget. Checking the post-advance position here -- before the line is acted on -- also
    // rejects a single oversized (but newline-terminated) header, which the per-header check below missed
    // because the empty-line / status-line branches return or `continue` before reaching it.
    if (_pos > maxResponseBytes) {
      return Status::TooLarge;
    }

    if (_state == State::StatusLine) {
      const auto sp1 = line.find(' ');
      if (sp1 == std::string_view::npos) {
        return Status::Error;
      }
      const http::Version version{line.substr(0, sp1)};
      if (!version.isValid()) {
        return Status::Error;
      }
      // HTTP/1.1 (and newer) default to keep-alive; HTTP/1.0 defaults to close.
      _http11 = version == http::HTTP_1_1;

      const std::string_view afterVersion = line.substr(sp1 + 1);
      const auto sp2 = afterVersion.find(' ');
      const std::string_view codeTok = sp2 == std::string_view::npos ? afterVersion : afterVersion.substr(0, sp2);
      const auto* begin = codeTok.data();
      const auto* end = begin + codeTok.size();
      const auto [ptr, ec] = std::from_chars(begin, end, _statusCode);
      if (ec != std::errc{} || ptr != end || _statusCode < 100 || _statusCode > 599) {
        return Status::Error;
      }
      resp.status(_statusCode);
      if (sp2 != std::string_view::npos) {
        resp.reason(TrimOws(afterVersion.substr(sp2 + 1)));
      }
      _state = State::Headers;
      continue;
    }

    // Headers state.
    if (line.empty()) {
      // End of header block. 1xx interim responses are discarded; restart for the final response.
      // status code should have been parsed already.
      assert(_statusCode >= 100 && _statusCode <= 599);
      if (_statusCode < 200) {
        _connectionCloseSeen = false;
        _connectionKeepAliveSeen = false;
        _hasContentLength = false;
        _chunked = false;
        _contentTypeLen = 0;
        _state = State::StatusLine;
        continue;
      }
      // decideFraming() only ever yields Complete (bodyless / zero-length) or NeedMore (a body follows).
      if (decideFraming() == Status::Complete) {
        return installBody(resp, std::string_view(buffer.data(), buffer.size()));
      }
      if (_streamHead) {
        return Status::HeadComplete;  // the next parse() call goes straight to the body
      }
      break;  // proceed to body parsing below
    }

    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      return Status::Error;
    }
    char* const lineFirst = buffer.data() + static_cast<std::size_t>(line.data() - bufferBegin);
    tolower(lineFirst, colon);
    const LowerAsciiKey name(line.substr(0, colon));
    const std::string_view value = TrimOws(line.substr(colon + 1));

    if (!http::IsValidHeaderValue(value)) {
      return Status::Error;
    }

    // Content-Type / Content-Length / Transfer-Encoding are normalized by HttpResponse::body()
    // (Content-Type + decoded Content-Length) and by de-framing (chunked), so they are consumed
    // locally rather than stored. Every other header, including otherwise-reserved ones such as Connection, Date,
    // Trailer and Upgrade, keeps its value. Names have already been normalized to lower-case above.
    if (name == http::ContentType) {
      // Remember where the value sits in the buffer instead of copying it; it is resolved at install time.
      _contentTypeOff = static_cast<std::size_t>(value.data() - buffer.data());
      _contentTypeLen = value.size();
    } else if (name == http::ContentLength) {
      const auto* pBeg = value.data();
      const auto* pEnd = pBeg + value.size();
      const auto [ptr, ec] = std::from_chars(pBeg, pEnd, _bodyRemaining);
      if (ec != std::errc{} || ptr != pEnd) {
        return Status::Error;
      }
      _hasContentLength = true;
    } else if (name == http::TransferEncoding) {
      _chunked = LastTransferEncodingIsChunked(value);
    } else {
      if (name == http::Connection) {
        ScanConnectionTokens(value, _connectionCloseSeen, _connectionKeepAliveSeen);
      } else if (name == http::ContentEncoding) {
        // Content codings are case-insensitive (RFC 9110 §8.4.1): lower-case them in place before storing.
        tolower(lineFirst + (value.data() - line.data()), value.size());
      } else if (!http::IsValidHeaderName(name)) {
        return Status::Error;
      }

      internal::HttpResponseAccess::HeaderAddLineUnchecked(resp, name, value);
    }
  }

  if (_state == State::Done) {
    return Status::Complete;
  }

  const Status bodyStatus = parseBody(std::string_view(buffer.data(), buffer.size()), eof, maxResponseBytes);
  if (bodyStatus == Status::Complete && !_streaming) {
    return installBody(resp, std::string_view(buffer.data(), buffer.size()));
  }
  return bodyStatus;
}

void Http1ResponseParser::discardStreamed(RawChars& buffer) noexcept {
  // Gathered chunks are always delivered before the status following them.
  assert(_streaming && !_gatheredPiece && _bodyBuf->empty());
  buffer.erase_front(_pos);
  _pos = 0;
  _bodyStart = 0;
}

}  // namespace aeronet
