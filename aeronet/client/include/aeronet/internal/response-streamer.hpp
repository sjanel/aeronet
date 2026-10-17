#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/encoding.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/response-body-sink.hpp"

namespace aeronet {

class HttpClientConfig;
class HttpResponse;

namespace internal {

struct HttpClientCodec;

constexpr bool IsRedirectStatus(http::StatusCode code) noexcept {
  return code == http::StatusCodeMovedPermanently || code == http::StatusCodeFound ||
         code == http::StatusCodeSeeOther || code == http::StatusCodeTemporaryRedirect ||
         code == http::StatusCodePermanentRedirect;
}

// Delivers the final response of a request to its ResponseBodySink, decoding a content-coded body as it arrives. The
// protocol engines call startResponse() once the head of a response announcing a body is received, then feed() with
// the received body bytes and finish() at its end. A response received whole is never installed in its HttpResponse:
// the protocol engines give its body with setBufferedBody(), and HttpClient delivers it with deliverBuffered() once it
// is known to be the final response. Inactive (no sink) for regular requests, whose bodies are buffered.
//
// A body is decoded incrementally when it has a single content coding, each received piece in one decoder call: at its
// end, the decoder tells whether the coded stream is complete. Stacked codings are buffered and decoded whole.
class ResponseStreamer {
 public:
  // Start a request whose final response goes to `sink`, or a regular one (nullptr). The configuration and the codec
  // (decoders, and its decompressOut buffer receiving the decoded bytes) are borrowed until the next call.
  void beginRequest(ResponseBodySink* sink, const HttpClientConfig& config, HttpClientCodec& codec) noexcept;

  [[nodiscard]] bool active() const noexcept { return _sink != nullptr; }

  // Whether the sink received a response head during the current request: from then on, it cannot be retried.
  [[nodiscard]] bool headDelivered() const noexcept { return _headDelivered; }

  // Prepare for one exchange of a sink request. A response that would then be followed (a redirect when
  // `redirectFollowed`) or retried (a status of retryStatuses when `statusRetried`) is buffered instead of delivered.
  void beginExchange(bool redirectFollowed, bool statusRetried) noexcept;

  // Called once the head of a final response announcing a body is received, `body` holding its raw Content-Type value
  // (empty when absent) and its Content-Length (ResponseBodyInfo::kUnknownSize when absent, or for chunked framing).
  // Sets `stream` when the body must go through feed() / finish(): onHead() was then called, and the
  // Content-Encoding header removed when the body is decoded. Clears it when the body must be buffered. Returns an
  // error (also reported by error()) when the sink refused the head (aborted), or when the content coding is not
  // supported (malformedResponse, without receiving the body).
  [[nodiscard]] HttpClientErrc startResponse(HttpResponse& head, ResponseBodyInfo body, bool& stream);

  // Deliver the next received bytes of the body (as received, so still content-coded). Returns false on failure, whose
  // cause is then given by error().
  [[nodiscard]] bool feed(std::string_view data);

  // Signal the end of the body. Returns false on failure (such as a truncated content coding), see error().
  [[nodiscard]] bool finish();

  [[nodiscard]] HttpClientErrc error() const noexcept { return _error; }

  // Keep the body of a response received whole (bodyless, or buffered by startResponse()) for deliverBuffered(). The
  // body stays where it is, in the client scratch buffers, which are not reused before; its raw Content-Type value is
  // copied (it may belong to a connection closed meanwhile).
  void setBufferedBody(std::string_view body, std::string_view contentType);

  // Deliver the final response received whole to the sink: `head` (without body), then the body given to
  // setBufferedBody(), without copy. Returns HttpClientErrc::aborted when the sink refused it.
  [[nodiscard]] HttpClientErrc deliverBuffered(const HttpResponse& head);

 private:
  enum class Coding : uint8_t { Streamed, Buffered, Unsupported };

  // Select the decoder of a single content coding (Streamed), or tell whether stacked codings are all supported
  // (Buffered) or not (Unsupported).
  Coding selectDecoder(std::string_view contentEncoding);

  // Decode `chunk`, appending the result to the decompressOut buffer and enforcing the decompression limits. An empty
  // final chunk checks that the coded stream is complete.
  bool decode(std::string_view chunk, bool finalChunk);

  bool deliver(std::string_view data);

  ResponseBodySink* _sink{nullptr};
  const HttpClientConfig* _config{nullptr};
  HttpClientCodec* _codec{nullptr};
  std::string_view _bufferedBody;   // see setBufferedBody()
  RawChars32 _bufferedContentType;  // raw Content-Type value of the response received whole
  void* _decoder{nullptr};          // context of the decoder of _coding
  std::size_t _compressedBytes{0};  // content-coded bytes decoded so far
  std::size_t _decodedBytes{0};
  HttpClientErrc _error{HttpClientErrc::noError};
  Encoding _encoding{Encoding::none};  // none: the body is delivered as received
  bool _bufferRedirects{false};
  bool _bufferRetries{false};
  bool _headDelivered{false};
};

}  // namespace internal
}  // namespace aeronet
