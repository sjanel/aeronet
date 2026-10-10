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
- A directory is served through its index file (`index.html` by default, see `withDefaultIndex()`), when it has one. Otherwise, it is answered `404`, unless `enableDirectoryIndex` is set: the handler then redirects `/dir` to `/dir/`, and lists the directory as an HTML page.
- Only `GET` and `HEAD` are served; other methods receive `405 Method Not Allowed`.

!!! warning
    The handler serves every regular file under the root that a request names, **dotfiles included**: `showHiddenFiles` only hides them from directory listings, so `/.env` or `/.git/config` are served if they exist. Symbolic links are followed, including links that point outside the root. Serve a directory that contains only public files, built for that purpose, never a source tree or a home directory.

## Transfers

Files larger than `inlineFileThresholdBytes` (128 KiB by default) are sent from the file: with `sendfile()` on plaintext connections, through kernel TLS when [kTLS](../protocols/tls.md#kernel-tls-ktls) is active, and otherwise by reading bounded chunks that are encrypted, or framed as HTTP/2 DATA frames. Smaller files are read into the response, which is faster for them. The file is never loaded entirely in memory, whatever its size.

The content type comes from, in order: the `contentTypeResolver` callback when it returns a non-empty value, the file extension (a table of common types), and `defaultContentType` (`application/octet-stream` by default).

Files are not compressed: store pre-compressed copies of large text assets if needed, see [Compression](compression.md#what-is-not-compressed).

## Caching and conditional requests

Each response carries a strong `ETag`, derived from the file size and modification time, and a `Last-Modified` header (disable them with `addEtag` and `addLastModified`). The handler evaluates `If-None-Match`, `If-Modified-Since`, `If-Match`, and `If-Unmodified-Since` (disable with `enableConditional`):

- a resource that did not change is answered `304 Not Modified`, without body;
- a failed precondition is answered `412 Precondition Failed`.

The handler does not set `Cache-Control`: add it, for instance in a [response middleware](middleware.md) that matches your asset naming (long lifetimes for fingerprinted files, `no-cache` for HTML).

To save formatting work, each handler keeps the formatted headers (`ETag`, `Last-Modified`, `Content-Type`) of up to `headerCacheCapacity` files (1024 by default, least recently used evicted, 0 to disable). Every request still checks the file size and modification time, so a modified file is never served with stale headers.

## Ranges

With `enableRange` (the default), the handler answers `Accept-Ranges: bytes` and serves byte ranges (RFC 9110 §14):

- **A single range** (`Range: bytes=0-499`, `bytes=500-`, `bytes=-500`) is answered `206 Partial Content` with `Content-Range`.
- **Several ranges** (`Range: bytes=0-99,200-299`) are answered `206` with a `multipart/byteranges` body, one part per range with its own `Content-Range`. Overlapping and adjacent ranges are merged first; when one range remains, it is sent as a single range.
- An unsatisfiable range is dropped; when no range is satisfiable, the answer is `416 Range Not Satisfiable` with `Content-Range: bytes */<size>`. A malformed `Range` is answered `416` too, and an unknown range unit is ignored.
- `If-Range` with a validator that no longer matches returns the full file with `200`.

Multipart range responses are assembled in memory, so two limits protect the server: more than `maxMultipartRanges` ranges (16 by default) are answered `416`, and a response that would exceed `maxMultipartBodySize` (32 MiB by default) is replaced by the full file with `200`.

## Configuration

| `StaticFileConfig` | Default | Effect |
| --- | --- | --- |
| `withDefaultIndex(name)` | `index.html` | File served for a directory; empty to disable. |
| `enableDirectoryIndex` | `false` | List directories without index file as HTML. |
| `maxEntriesToList` | 10,000 | Maximum entries in a listing; longer listings are truncated and carry `x-directory-listing-truncated: 1`. |
| `showHiddenFiles` | `false` | Show dotfiles in listings (they are always served when requested). |
| `withDirectoryListingCss(css)`, `directoryIndexRenderer` | default style, built-in renderer | Customize listings. |
| `enableRange`, `maxMultipartRanges`, `maxMultipartBodySize` | `true`, 16, 32 MiB | Range requests. |
| `enableConditional`, `addEtag`, `addLastModified` | `true` | Validators and conditional requests. |
| `contentTypeResolver`, `withDefaultContentType(type)` | none, `application/octet-stream` | Content type resolution. The resolver must return views to storage that outlives the handler. |
| `withInlineFileThresholdBytes(n)` | 128 KiB | Files up to this size are read into the response. |
| `withHeaderCacheCapacity(n)` | 1024 | Formatted header cache size. |

## Tests

- Path resolution, ranges, multipart assembly, conditionals, and header cache: [static-file-handler_test.cpp](../../aeronet/server/test/static-file-handler_test.cpp).
- End to end, including multi-range responses and `If-Range`: [tests/http-core_test.cpp](../../tests/http-core_test.cpp).
