# Static files

`StaticFileHandler` serves a directory tree: it maps request paths to files under a root directory, sends them without copying them through user space when possible, and implements the HTTP caching and range semantics of RFC 9110 (conditional requests, `ETag`, `Last-Modified`, single and multiple byte ranges). It is an ordinary handler, registered like any other route.

```cpp
#include <utility>

StaticFileConfig staticConfig;
staticConfig.enableDirectoryIndex = true;  // HTML listing for directories without index.html

Router router;
router.setDefault(StaticFileHandler("/var/www/site", std::move(staticConfig)));

SingleHttpServer server(HttpServerConfig{}.withPort(8080), std::move(router));
server.run();
```

The [static file example](../../examples/static-file.cpp) serves any directory given on its command line:

```bash
cmake --build build --target aeronet-static-example
./build/examples/aeronet-static-example 8080 ./examples/static-assets
curl -i http://localhost:8080/somefile.txt
curl -i -H "Range: bytes=0-3" http://localhost:8080/somefile.txt
```

## Mapping paths to files

The handler resolves the **whole request path** under its root directory: with root `/var/www/site`, `GET /css/site.css` serves `/var/www/site/css/site.css`. The route prefix is not removed, so a handler registered on `/assets/*` with root `/var/www/site` serves `/assets/app.js` from `/var/www/site/assets/app.js`.

- The path is percent-decoded before resolution, `.` segments are ignored, and any `..` segment is answered `404`, so a request cannot leave the root through its path.
- Hidden files and directories, whose name starts with a dot, are answered `404` and left out of listings, so `/.env` or `/.git/config` are not served. `.well-known` is the exception: [RFC 8615](https://www.rfc-editor.org/rfc/rfc8615) reserves it for public metadata such as ACME challenges or `security.txt`, so it is served and listed, while the dotfiles it contains stay hidden. Set `showHiddenFiles` to serve and list every dotfile.
- Symbolic links are followed as long as their target is inside the root directory: a file or directory reached through a link that leads outside of the root is answered `404`. The check resolves the links of the path once per file, then is remembered in the [header cache](#caching-and-conditional-requests) along with the identity of the file (device and inode), so it costs nothing on the following requests and runs again when the path leads to another file. A directory listing leaves out the links answered `404` (leading outside of the root, to a hidden name, or dangling), without describing their targets, and lists the other links as their targets: a link to a directory as a directory.
- The hidden-file rule also applies to the resolved path: a link to a dotfile (or into a hidden directory) inside the root is answered `404`, and so is a Windows short name such as `/GIT~1/config` for `/.git/config`. Only the part below the root counts: the root directory itself may be hidden.
- The path is resolved after the file was opened, so the server also checks that the resolved path designates the opened file (same device and inode, or volume and file index on Windows). Otherwise a link retargeted between the two steps could let a file outside of the root be served under the check of a file inside it.
- Only regular files and directories are served: a FIFO, a socket or a device file is answered `404`. The type is checked on the path, then again on the opened file, which is opened without blocking (`O_NONBLOCK`): a file replaced by a FIFO in between cannot block the event loop.
- A directory is served at its URL ending with a slash, against which the relative links of its page resolve: `/dir` is redirected to `/dir/` (its `Location` re-encoding the decoded path, whatever bytes the name of the directory holds, and starting with `/.` for a path starting with `//`, which a client would take for another host). `/dir/` is served through its index file (`index.html` by default, see `withDefaultIndex()`), when it has one. Otherwise, it is answered `404`, unless `enableDirectoryIndex` is set: the directory is then listed as an HTML page. A directory that would be answered `404` once redirected (a link leading outside of the root) is answered `404` without redirect.
- Only `GET` and `HEAD` are served; other methods receive `405 Method Not Allowed`.

!!! tip
    Serve a directory that contains only public files, built for that purpose, rather than a source tree or a home directory: every regular file inside it that is not hidden can be downloaded.

## Transfers

Files larger than `inlineFileThresholdBytes` (128 KiB by default, at most `StaticFileConfig::kMaxInMemoryBodyBytes`, 2 MiB) are sent from the file: with `sendfile()` on plaintext connections, through kernel TLS when [kTLS](../protocols/tls.md#kernel-tls-ktls) is active, and otherwise by reading bounded chunks that are encrypted, or framed as HTTP/2 DATA frames. The file is never loaded entirely in memory, whatever its size. Smaller files are read into the response, which is faster for them.

The size of a file is read when it is opened, and announced in the `Content-Length` of its response. A file that shrinks while it is sent (truncated in place, by a log rotation with `copytruncate` for instance) cannot complete it: the server closes the HTTP/1.1 connection after the bytes sent, or resets the HTTP/2 stream, so that the client knows the response is incomplete.

The content type comes from, in order: the `contentTypeResolver` callback when it returns a non-empty value, the file extension (a table of common types), and `defaultContentType` (`application/octet-stream` by default).

`File::detectedContentType()` exposes the same extension-based detection to your own code (for instance when sending a file with `HttpResponse::file()` or `HttpResponseWriter::file()`). Its built-in mapping includes common C and C++ extensions such as `c`, `h`, `cpp`, `hpp`, and `cc`.

The files read into the response are compressed like any body in memory, when the server has [compression](compression.md) enabled and the client accepts a coding: their strong `ETag` is then made weak (see [Entity tags](compression.md#entity-tags)). The files sent from the file are never compressed, see [What is not compressed](compression.md#what-is-not-compressed).

## Sending a single file

To send one file from a handler instead of serving a directory, use `HttpResponse::file()` (fixed response) or `HttpResponseWriter::file()` (streaming response). The [sendfile example](../../examples/sendfile.cpp) exposes two endpoints:

- `GET /static` - returns the contents of a file using `HttpResponse::file` (fixed response).
- `GET /stream` - returns the contents of a file using `HttpResponseWriter::file` (streaming writer API).

Build the examples and run it:

```bash
cmake -S . -B build -D CMAKE_BUILD_TYPE=Release
cmake --build build
./build/examples/aeronet-sendfile 8080 /path/to/file
```

If the file path argument is omitted, the example creates a small temporary file in `/tmp` and serves it. Fetch the file with curl:

```bash
curl -i http://localhost:8080/static
curl -i http://localhost:8080/stream
```

The example demonstrates both the fixed-response path (the server synthesizes a `content-length` header) and the streaming writer path. For plaintext sockets, the server uses the kernel `sendfile(2)` system call for zero-copy transmission. When TLS is enabled, the example exercises the TLS fallback that `pread()`s into the connection buffer and writes through the TLS transport.

## Caching and conditional requests

Each response carries a strong `ETag`, derived from the file size and modification time, and a `Last-Modified` header (disable them with `addEtag` and `addLastModified`). The handler evaluates `If-Match`, `If-Unmodified-Since`, `If-None-Match`, and `If-Modified-Since`, in the order of RFC 9110 §13.2.2 (disable with `enableConditional`):

- a resource that did not change is answered `304 Not Modified`, without body. It carries the validators of the `200` it stands for (RFC 9110 §15.4.5): for a file that the server would compress, the weak `ETag` and `Vary: accept-encoding` of the compressed response;
- a failed precondition is answered `412 Precondition Failed`.

`If-Match` uses the strong comparison, a weak tag never matching, and `If-None-Match` the weak one, so that the weak `ETag` of a compressed response matches the file. `If-Unmodified-Since` is ignored when the request has `If-Match`, and `If-Modified-Since` when it has `If-None-Match`, as an invalid date is. The dates are compared at the one-second resolution of `Last-Modified`, the value that clients send back, although file systems keep a finer modification time. They are accepted in the three HTTP-date formats (RFC 9110 §5.6.7): `Sun, 06 Nov 1994 08:49:37 GMT`, and the obsolete `Sunday, 06-Nov-94 08:49:37 GMT` and `Sun Nov  6 08:49:37 1994`. A modification time beyond the range of the system clock (after 2262 with its nanosecond ticks) is clamped to it.

The handler does not set `Cache-Control`: add it, for instance in a [response middleware](middleware.md) that matches your asset naming (long lifetimes for fingerprinted files, `no-cache` for HTML).

To save formatting work, each handler keeps the formatted headers (`ETag`, `Last-Modified`, `Content-Type`) of up to `headerCacheCapacity` files (1024 by default, least recently used evicted, 0 to disable). Every request still checks the file size and modification time, so a modified file is never served with stale headers. The cache also remembers that the file is inside the root directory: without cache, the symbolic links of the path are resolved on every request.

## Ranges

With `enableRange` (the default), the handler answers `Accept-Ranges: bytes` and serves byte ranges (RFC 9110 §14):

- **A single range** (`Range: bytes=0-499`, `bytes=500-`, `bytes=-500`) is answered `206 Partial Content` with `Content-Range`.
- **Several ranges** (`Range: bytes=0-99,200-299`) are answered `206` with a `multipart/byteranges` body, one part per range with its own `Content-Range`. Overlapping and adjacent ranges are merged first; when one range remains, it is sent as a single range.
- An unsatisfiable range is dropped; when no range is satisfiable, the answer is `416 Range Not Satisfiable` with `Content-Range: bytes */<size>`. A malformed `Range` is answered `416` too, and an unknown range unit is ignored. The positions have no maximum: a number beyond 64 bits stands for the end of the file. An empty file has a single satisfiable range, a suffix (`bytes=-500`), which no `Content-Range` can describe: the `Range` is then ignored and the empty file answered `200`.
- `If-Range` returns the full file with `200` unless its validator exactly matches the current one: the `ETag` (strong comparison), or the `Last-Modified` date (a later date does not match: the file may have been replaced by an older version).

Multipart range responses are assembled in memory, so two limits protect the server: more than `maxMultipartRanges` ranges (16 by default) are answered `416`, and a response that would exceed `maxMultipartBodySize` (1 MiB by default) is replaced by the full file with `200`, sent from the file. `maxMultipartBodySize` and `inlineFileThresholdBytes` cannot exceed `StaticFileConfig::kMaxInMemoryBodyBytes` (2 MiB, the handler throws `std::invalid_argument` otherwise): half of the default output limits of a connection, `maxOutboundBufferBytes` and the `maxStreamPendingBytes` of an HTTP/2 stream. A response over these limits is answered `503` over HTTP/2: keep them at least twice as large as these two fields.

## Configuration

| `StaticFileConfig` | Default | Effect |
| --- | --- | --- |
| `withDefaultIndex(name)` | `index.html` | File served for a directory; empty to disable. |
| `enableDirectoryIndex` | `false` | List directories without index file as HTML. |
| `maxEntriesToList` | 10,000 | Maximum entries in a listing; longer listings are truncated to their first entries (directories first, then by name) and carry `x-directory-listing-truncated: 1`. The entries beyond the limit cost a comparison, without reading their metadata. |
| `showHiddenFiles` | `false` | Serve and list dotfiles. When `false`, they are answered `404` and left out of listings, except `.well-known`. |
| `withDirectoryListingCss(css)`, `directoryIndexRenderer` | default style, built-in renderer | Customize listings. |
| `enableRange`, `maxMultipartRanges`, `maxMultipartBodySize` | `true`, 16, 1 MiB | Range requests. `maxMultipartBodySize` is at most 2 MiB. |
| `enableConditional`, `addEtag`, `addLastModified` | `true` | Validators and conditional requests. |
| `contentTypeResolver`, `withDefaultContentType(type)` | none, `application/octet-stream` | Content type resolution. The resolver must return views to storage that outlives the handler. |
| `withInlineFileThresholdBytes(n)` | 128 KiB | Files up to this size are read into the response (and may be compressed), at most 2 MiB. |
| `withHeaderCacheCapacity(n)` | 1024 | Formatted header cache size. |

## Tests

- Path resolution, hidden files, symbolic links, ranges, multipart assembly, conditionals, and header cache: [static-file-handler_test.cpp](../../aeronet/server/test/static-file-handler_test.cpp); the `Location` of the directory redirect (`DirectoryRedirectLocation*`), the redirect of a directory with an index file (`IndexOfDirectoryWithoutTrailingSlashRedirects`), the dates compared at the second (`IfModifiedSinceEqualToLastModifiedIsNotModified`, `IfUnmodifiedSinceEqualToLastModifiedSucceeds`, `IfRangeDate*`), the order and comparisons of the preconditions (`IfNoneMatchUsesWeakComparison`, `IfUnmodifiedSinceIgnoredWhenIfMatchIsPresent`, `InvalidIfUnmodifiedSinceStillEvaluatesTheOtherPreconditions`, `ObsoleteHttpDateFormatsAreAccepted`), the validators of a `304` (`NotModifiedCarriesTheValidatorsOfTheCompressedResponse`), ranges of empty and sparse files (`RangeRequestsOnEmptyFile`, `RangesBeyond4GiB`, `RangeBoundsBeyond64BitsAreValid`), listings (`TruncatedListingKeepsTheFirstEntriesAtLinearCost`, `ListingLeavesOutLinksAnswered404`), a FIFO swapped in before the opening (`FileReplacedByFifoBeforeOpeningIsNotServed`) and the modification times beyond the clock (`ModificationTimeBeyondClockRangeIsClamped`).
- End to end, including multi-range responses and `If-Range`: [tests/http-core_test.cpp](../../tests/http-core_test.cpp); a file truncated while sent, over HTTP/1.1 (`HttpRangeStatic.FileTruncatedAfterItsSizeWasReadClosesTheConnection`), TLS (`HttpTlsStaticFile.FileTruncatedAfterItsSizeWasReadClosesTheConnection` in [tests/http-tls-io_test.cpp](../../tests/http-tls-io_test.cpp)) and HTTP/2, and multi-range responses over both protocols (`ProtocolParity.TruncatedFileResetsTheHttp2Stream`, `ProtocolParity.MultiRangeOfLargeFile` in [tests/http-protocol-parity_test.cpp](../../tests/http-protocol-parity_test.cpp)); revalidation of compressed files (`HttpCompression.CompressedStaticFileRevalidatesWithItsWeakETag`, `HttpCompression.NotModifiedOfCompressedStaticFileCarriesItsValidators` in [tests/http-compression_test.cpp](../../tests/http-compression_test.cpp)).
- The end of a file before the length of its response, closing the connection: `ConnectionStateSendfileTest.EndOfFileBeforeLengthCloses` and `TlsPreadEndOfFileBeforeLengthCloses` in [connection-state_test.cpp](../../aeronet/server/test/connection-state_test.cpp); HTTP-date formats: [timestring_test.cpp](../../aeronet/tech/test/timestring_test.cpp).
