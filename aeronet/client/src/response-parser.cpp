#include "response-parser.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet {

namespace {

// True if the last token of a (possibly comma-separated) Transfer-Encoding value is "chunked".
constexpr bool LastTransferEncodingIsChunked(std::string_view value) {
  const auto comma = value.rfind(',');
  std::string_view last = comma == std::string_view::npos ? value : value.substr(comma + 1);
  return CaseInsensitiveEqual(TrimOws(last), http::chunked);
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

void ResponseParser::reset(bool headRequest) noexcept {
  _bodyBuf.clear();
  _pos = 0;
  _bodyStart = 0;
  _contentTypeOff = 0;
  _contentTypeLen = 0;
  _bodyRemaining = 0;
  _framing = Framing::None;
  _state = State::StatusLine;
  _statusCode = 0;
  _headRequest = headRequest;
  _keepAlive = false;
  _http11 = false;
  _connectionCloseSeen = false;
  _connectionKeepAliveSeen = false;
  _hasContentLength = false;
  _chunked = false;
}

ResponseParser::Status ResponseParser::installBody(HttpResponse& resp, std::string_view buffer) const {
  if (_framing == Framing::None) {
    return Status::Complete;  // bodyless response (HEAD / 204 / 304 / 1xx)
  }
  const std::string_view contentType =
      _contentTypeLen == 0 ? http::ContentTypeApplicationOctetStream : buffer.substr(_contentTypeOff, _contentTypeLen);
  // Chunked bodies are reassembled in _bodyBuf; Length / UntilClose bodies are still contiguous in the
  // receive buffer, so they are installed straight from it (resp.body() performs the single owning copy).
  const std::string_view body =
      _framing == Framing::Chunked ? std::string_view(_bodyBuf) : buffer.substr(_bodyStart, _pos - _bodyStart);

  // Automatic decompression: decode straight from `body` (receive buffer / de-framed chunks) into the
  // borrowed scratch buffer, drop the now-stale Content-Encoding header (the body is not installed yet, so
  // removal is cheap and legal), then install the decoded bytes. No intermediate copy of the compressed body.
  if (_decode.state != nullptr) {
    const std::string_view contentEncoding = resp.headerValueOrEmpty(http::ContentEncoding);
    if (!contentEncoding.empty()) {
      std::string_view decoded;
      const auto res = internal::HttpCodec::DecompressFullBody(*_decode.state, *_decode.config, contentEncoding, body,
                                                               *_decode.out, *_decode.tmp, decoded);
      if (res.status != http::StatusCodeOK) {
        return Status::Error;
      }
      resp.headerRemoveLine(http::ContentEncoding);  // body not installed yet => legal & cheap
      resp.body(decoded, contentType);
      return Status::Complete;
    }
  }
  resp.body(body, contentType);
  return Status::Complete;
}

ResponseParser::Status ResponseParser::decideFraming() {
  // Connection persistence: HTTP/1.1 defaults to keep-alive, HTTP/1.0 defaults to close.
  bool keepAlive = _http11;
  if (_connectionCloseSeen) {
    keepAlive = false;
  } else if (_connectionKeepAliveSeen) {
    keepAlive = true;
  }

  // The cursor sits on the first body byte (just past the blank line ending the header block).
  _bodyStart = _pos;

  const http::StatusCode code = _statusCode;
  const bool bodyless = _headRequest || code == http::StatusCodeNoContent || code == http::StatusCodeNotModified ||
                        (code >= 100 && code < 200);

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

ResponseParser::Status ResponseParser::parseBody(std::string_view buffer, bool eof, std::size_t maxResponseBytes) {
  switch (_framing) {
    case Framing::Length: {
      const std::size_t available = buffer.size() - _pos;
      const std::size_t take = available < _bodyRemaining ? available : _bodyRemaining;
      // The body stays in place in the receive buffer; only the cursor advances (zero-copy until install).
      if ((_pos - _bodyStart) + take > maxResponseBytes) {
        return Status::Error;
      }
      _pos += take;
      _bodyRemaining -= take;
      if (_bodyRemaining == 0) {
        _state = State::Done;
        return Status::Complete;
      }
      return eof ? Status::Error : Status::NeedMore;
    }

    case Framing::UntilClose: {
      const std::size_t available = buffer.size() - _pos;
      if ((_pos - _bodyStart) + available > maxResponseBytes) {
        return Status::Error;
      }
      _pos += available;
      if (eof) {
        _state = State::Done;
        return Status::Complete;
      }
      return Status::NeedMore;
    }

    case Framing::Chunked: {
      for (;;) {
        if (_state == State::BodyChunkSize) {
          const auto nl = buffer.find('\n', _pos);
          if (nl == std::string_view::npos) {
            return eof ? Status::Error : Status::NeedMore;
          }
          std::string_view line = buffer.substr(_pos, nl - _pos);
          if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
          }
          if (const auto semi = line.find(';'); semi != std::string_view::npos) {
            line = line.substr(0, semi);  // strip chunk extensions
          }
          line = TrimOws(line);
          std::size_t chunkSize = 0;
          const auto* begin = line.data();
          const auto* end = begin + line.size();
          const auto [ptr, ec] = std::from_chars(begin, end, chunkSize, 16);
          if (ec != std::errc{} || ptr != end) {
            return Status::Error;
          }
          _pos = nl + 1;
          if (chunkSize == 0) {
            _state = State::BodyChunkTrailers;
          } else {
            _bodyRemaining = chunkSize;
            _state = State::BodyChunkData;
          }
          continue;
        }

        if (_state == State::BodyChunkData) {
          const std::size_t available = buffer.size() - _pos;
          const std::size_t take = available < _bodyRemaining ? available : _bodyRemaining;
          if (_bodyBuf.size() + take > maxResponseBytes) {
            return Status::Error;
          }
          _bodyBuf.append(buffer.data() + _pos, take);
          _pos += take;
          _bodyRemaining -= take;
          if (_bodyRemaining != 0) {
            return eof ? Status::Error : Status::NeedMore;
          }
          _state = State::BodyChunkCrlf;
          continue;
        }

        if (_state == State::BodyChunkCrlf) {
          const auto nl = buffer.find('\n', _pos);
          if (nl == std::string_view::npos) {
            return eof ? Status::Error : Status::NeedMore;
          }
          _pos = nl + 1;
          _state = State::BodyChunkSize;
          continue;
        }

        if (_state == State::BodyChunkTrailers) {
          // Consume trailer lines until an empty line. Trailers are not surfaced.
          for (;;) {
            const auto nl = buffer.find('\n', _pos);
            if (nl == std::string_view::npos) {
              return eof ? Status::Error : Status::NeedMore;
            }
            std::string_view line = buffer.substr(_pos, nl - _pos);
            const bool emptyLine = line.empty() || line == "\r";
            _pos = nl + 1;
            if (emptyLine) {
              _state = State::Done;
              return Status::Complete;
            }
          }
        }

        // The four chunked sub-states above each return or `continue`; control never falls through here.
        std::unreachable();
      }
    }

    case Framing::None:
    default:
      // parse() resolves bodyless / zero-length framings (None) to Complete before any body parsing, so
      // parseBody() is only ever entered for Length / UntilClose / Chunked. None / default is unreachable.
      std::unreachable();
  }
}

ResponseParser::Status ResponseParser::parse(std::string_view buffer, bool eof, HttpResponse& resp,
                                             std::size_t maxResponseBytes) {
  // --- Status line + headers ---
  while (_state == State::StatusLine || _state == State::Headers) {
    const auto nl = buffer.find('\n', _pos);
    if (nl == std::string_view::npos) {
      // No complete line yet. Everything buffered so far is head bytes (the body can only begin after the
      // empty line we have not seen), so bound the buffer itself: otherwise a peer streaming an endless
      // header line keeps `_pos` pinned at the line start and grows the receive buffer without limit.
      if (buffer.size() > maxResponseBytes) {
        return Status::Error;
      }
      return eof ? Status::Error : Status::NeedMore;
    }
    std::string_view line = buffer.substr(_pos, nl - _pos);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    _pos = nl + 1;
    // Every consumed head line (status line, a header, or the terminating empty line) counts against the
    // total response budget. Checking the post-advance position here -- before the line is acted on -- also
    // rejects a single oversized (but newline-terminated) header, which the per-header check below missed
    // because the empty-line / status-line branches return or `continue` before reaching it.
    if (_pos > maxResponseBytes) {
      return Status::Error;
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
      _http11 = version != http::HTTP_1_0;

      std::string_view afterVersion = line.substr(sp1 + 1);
      const auto sp2 = afterVersion.find(' ');
      std::string_view codeTok = sp2 == std::string_view::npos ? afterVersion : afterVersion.substr(0, sp2);
      uint32_t code = 0;
      const auto* begin = codeTok.data();
      const auto* end = begin + codeTok.size();
      const auto [ptr, ec] = std::from_chars(begin, end, code);
      if (ec != std::errc{} || ptr != end || code < 100 || code > 599) {
        return Status::Error;
      }
      _statusCode = static_cast<http::StatusCode>(code);
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
      if (_statusCode >= 100 && _statusCode < 200) {
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
        return installBody(resp, buffer);
      }
      break;  // proceed to body parsing below
    }

    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      return Status::Error;
    }
    const std::string_view name = line.substr(0, colon);
    const std::string_view value = TrimOws(line.substr(colon + 1));

    // Content-Type / Content-Length / Transfer-Encoding are normalized by HttpResponse::body()
    // (Content-Type + decoded Content-Length) and by de-framing (chunked), so they are consumed
    // locally rather than stored. Every other header - including otherwise-reserved ones such as
    // Connection, Date, Trailer, Upgrade - is stored verbatim via rawHeader() so the received
    // message is represented losslessly.
    if (CaseInsensitiveEqual(name, http::ContentType)) {
      // Remember where the value sits in the buffer instead of copying it; it is resolved at install time.
      _contentTypeOff = static_cast<std::size_t>(value.data() - buffer.data());
      _contentTypeLen = value.size();
    } else if (CaseInsensitiveEqual(name, http::ContentLength)) {
      std::size_t len = 0;
      const auto* pValue = value.data();
      const auto* pEnd = pValue + value.size();
      const auto [ptr, ec] = std::from_chars(pValue, pEnd, len);
      if (ec != std::errc{} || ptr != pEnd) {
        return Status::Error;
      }
      _hasContentLength = true;
      _bodyRemaining = len;
    } else if (CaseInsensitiveEqual(name, http::TransferEncoding)) {
      _chunked = LastTransferEncodingIsChunked(value);
    } else {
      if (CaseInsensitiveEqual(name, http::Connection)) {
        ScanConnectionTokens(value, _connectionCloseSeen, _connectionKeepAliveSeen);
      }
      resp.rawHeader(name, value);
    }
  }

  if (_state == State::Done) {
    return Status::Complete;
  }

  const Status bodyStatus = parseBody(buffer, eof, maxResponseBytes);
  if (bodyStatus == Status::Complete) {
    return installBody(resp, buffer);
  }
  return bodyStatus;
}

}  // namespace aeronet
