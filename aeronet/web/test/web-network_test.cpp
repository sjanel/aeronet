// The requests of the scripts against local servers: fetch() and its Headers, Request, Response and AbortController,
// XMLHttpRequest, navigator.sendBeacon(), their order among the tasks of the page loop and LoadState::NetworkIdle, the
// CORS protocol (preflights, opaque responses, exposed headers), the credentials and SameSite cookies of the
// subresources, the bytecode cache of the scripts, and URL, URLSearchParams, TextEncoder, TextDecoder.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include "../src/cors.hpp"
#include "aeronet/cookie-jar.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/sys-test-support.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/test_server_tls_fixture.hpp"
#endif
#include "aeronet/web-client-config.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"
#include "web-test-util.hpp"

namespace aeronet {
namespace {

using std::chrono::milliseconds;

// The requests received by the servers, counted from their threads.
std::atomic<int> gPreflights{0};
std::atomic<int> gBeacons{0};
std::atomic<int> gCounted{0};  // the requests to /api with count=1, but the preflights
std::atomic<int> gScriptVersion{0};
std::mutex gMutex;
std::string gRequestHeaders;  // the Access-Control-Request-Headers of the last preflight, guarded by gMutex

std::string LastPreflightHeaders() {
  const std::scoped_lock lock(gMutex);
  return gRequestHeaders;
}

// 'str' as a JSON string.
std::string JsonString(std::string_view str) {
  std::string json = "\"";
  for (const char ch : str) {
    if (ch == '"' || ch == '\\') {
      json.push_back('\\');
      json.push_back(ch);
    } else if (static_cast<unsigned char>(ch) < 0x20U) {
      static constexpr char kHex[] = "0123456789abcdef";
      json.append("\\u00").push_back(kHex[static_cast<unsigned char>(ch) >> 4U]);
      json.push_back(kHex[static_cast<unsigned char>(ch) & 0xFU]);
    } else {
      json.push_back(ch);
    }
  }
  json.push_back('"');
  return json;
}

// Adds to 'resp' the CORS headers of the query parameters acao ('origin' echoes the Origin of the request), acac, acam,
// acah and aceh of 'req'.
void AddCorsHeaders(const HttpRequestView& req, HttpResponse& resp) {
  const std::string_view acao = req.queryParamValueOrEmpty("acao");
  if (!acao.empty()) {
    resp.headerAddLine("access-control-allow-origin", acao == "origin" ? req.headerValueOrEmpty("origin") : acao);
  }
  for (const auto& [param, header] :
       {std::pair{"acac", "access-control-allow-credentials"}, std::pair{"acam", "access-control-allow-methods"},
        std::pair{"acah", "access-control-allow-headers"}, std::pair{"aceh", "access-control-expose-headers"}}) {
    if (const auto value = req.queryParamValue(param)) {
      resp.headerAddLine(LowerAsciiKey(std::string_view(header)), *value);
    }
  }
}

// A redirect to the query parameter 'to' of 'req', of the status of its query parameter 'statusParam' ('defaultStatus'
// without it), with its CORS headers (see AddCorsHeaders()).
HttpResponse Redirect(const HttpRequestView& req, std::string_view statusParam, http::StatusCode defaultStatus) {
  const std::string_view status = req.queryParamValueOrEmpty(statusParam);
  HttpResponse resp =
      req.makeResponse(status.empty() ? defaultStatus : static_cast<http::StatusCode>(std::stoi(std::string(status))));
  resp.location(req.queryParamValue("to").value_or("/"));
  AddCorsHeaders(req, resp);
  return resp;
}

// /api, for every method: answers the request as JSON {"method", "target", "headers", "body"}, or the 'body' query
// parameter as is, with the status 'status' and its reason phrase 'reason', the Content-Type 'type', the CORS headers
// of its query parameters (see AddCorsHeaders()), and the Set-Cookie 'cookie'. With a 'to' query parameter, the methods
// other than OPTIONS are redirected there with the status 'redirect' (307 by default), and the preflights answered.
HttpResponse Api(const HttpRequestView& req) {
  if (req.method() == http::Method::OPTIONS) {
    ++gPreflights;
    const std::scoped_lock lock(gMutex);
    gRequestHeaders = req.headerValueOrEmpty("access-control-request-headers");
  } else {
    if (req.queryParamValueOrEmpty("count") == "1") {
      ++gCounted;
    }
    if (req.queryParamValue("to")) {
      return Redirect(req, "redirect", http::StatusCodeTemporaryRedirect);
    }
  }
  if (req.queryParamValueOrEmpty("beacon") == "1") {
    ++gBeacons;
  }
  std::string body;
  if (const auto fixed = req.queryParamValue("body")) {
    body = *fixed;
  } else {
    body = R"({"method":)" + JsonString(http::MethodToStr(req.method())) + R"(,"target":)" +
           JsonString(req.queryParamValueOrEmpty("path")) + R"(,"headers":{)";
    bool first = true;
    for (const auto& [name, value] : req.headers()) {
      body.append(first ? "" : ",").append(JsonString(name)).append(":").append(JsonString(value));
      first = false;
    }
    body.append(R"(},"body":)").append(JsonString(req.body())).append("}");
  }
  const std::string_view status = req.queryParamValueOrEmpty("status");
  HttpResponse resp = req.makeResponse(
      status.empty() ? http::StatusCodeOK : static_cast<http::StatusCode>(std::stoi(std::string(status))), body,
      req.queryParamValue("type").value_or(http::ContentTypeApplicationJson));
  AddCorsHeaders(req, resp);
  if (const auto reason = req.queryParamValue("reason")) {
    resp.reason(*reason);
  }
  if (const auto cookie = req.queryParamValue("cookie")) {
    resp.headerAddLine(http::SetCookie, *cookie);
  }
  resp.headerAddLine("x-custom", "custom");
  return resp;
}

test::TestServer MakeServer() {
  test::TestServer server = test::MakePageServer();
  auto router = server.router();
  router.setPath(http::Method::GET | http::Method::POST | http::Method::PUT | http::Method::DELETE |
                     http::Method::PATCH | http::Method::OPTIONS | http::Method::HEAD,
                 "/api", Api);
  // A script of the cookies sent with it, and of its Referer.
  router.setPath(http::Method::GET, "/cookies.js", [](const HttpRequestView& req) {
    return req.makeResponse("window.scriptCookies = " + JsonString(req.headerValueOrEmpty(http::Cookie)) +
                                "; window.scriptReferer = " + JsonString(req.headerValueOrEmpty("referer")),
                            "text/javascript");
  });
  // A script whose content changes at each request (its version), larger than the smallest cached script.
  router.setPath(http::Method::GET, "/version.js", [](const HttpRequestView& req) {
    return req.makeResponse("window.version = " + std::to_string(++gScriptVersion) + "; //" + std::string(2000, 'v'),
                            "text/javascript");
  });
  // Every method, the preflights too.
  router.setPath(http::Method::GET | http::Method::POST | http::Method::PUT | http::Method::OPTIONS, "/redirect",
                 [](const HttpRequestView& req) { return Redirect(req, "status", http::StatusCodeFound); });
  // 'size' bytes.
  router.setPath(http::Method::GET, "/large", [](const HttpRequestView& req) {
    return req.makeResponse(std::string(std::stoul(std::string(req.queryParamValueOrEmpty("size"))), 'a'),
                            "text/plain");
  });
  return server;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gServer = MakeServer();
// Another origin of the same site (127.0.0.1, another port), and of another site through localhost.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gOther = MakeServer();

// The configuration of the clients of these tests. A JavaScript call takes much more native stack with the sanitizers
// (about 70 KiB, see WebRuntimeTest.StackLimit), and the requests of an async function nest about 15 of them (fetch(),
// Request, Headers...): above the default limit of 1 MiB.
WebClientConfig Config() {
  WebClientConfig config;
  config.maxScriptStackSize = std::size_t{4} << 20U;
  return config;
}

std::string Url(std::string_view path) { return test::ServerUrl(gServer) + std::string(path); }

std::string SameSiteUrl(std::string_view path) { return test::ServerUrl(gOther) + std::string(path); }

std::string CrossSiteUrl(std::string_view path) { return test::ServerUrl(gOther, "localhost") + std::string(path); }

// The page of the script 'code', run until 'options' stop it.
WebPage RunScript(WebClient& client, std::string_view code, const NavigateOptions& options = {}) {
  return test::Navigate(client, test::PageUrl(gServer, "<script>" + std::string(code) + "</script>"), options);
}

// The JSON of window.out in the page of the script 'code'.
std::string Out(WebClient& client, std::string_view code, const NavigateOptions& options = {}) {
  WebPage page = RunScript(client, code, options);
  for (const std::string_view error : page.uncaughtErrors()) {
    ADD_FAILURE() << error;
  }
  return test::Eval(page, "out");
}

// --- fetch() ---

TEST(WebNetworkTest, FetchText) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(fetch('/api?body=hello&type=text/plain&status=201&reason=Created').then(async r => {
    out = [r.status, r.ok, r.statusText, r.type, r.redirected, r.url.endsWith('&status=201&reason=Created'),
      r.headers.get('content-type'), r.headers.get('x-custom'), r.bodyUsed, await r.text(), r.bodyUsed];
  }))"),
            R"([201,true,"Created","basic",false,true,"text/plain","custom",false,"hello",true])");
}

TEST(WebNetworkTest, FetchJsonAndArrayBuffer) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(Promise.all([
    fetch('/api?body=' + encodeURIComponent('{"a":[1,2]}')).then(r => r.json()),
    fetch('/api?body=abc').then(r => r.arrayBuffer()).then(b => [b instanceof ArrayBuffer, b.byteLength]),
    fetch('/api?body=xyz').then(r => r.bytes()).then(b => [...b]),
    fetch('/api?body=').then(r => r.text()),
    fetch('/api?body=x').then(r => r.blob()).catch(e => e.name),
  ]).then(results => { out = results; }))"),
            R"([{"a":[1,2]},[true,3],[120,121,122],"","TypeError"])");
}

TEST(WebNetworkTest, FetchSendsTheBodyAndHeadersOfTheScript) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(const echo = init => fetch('/api', init).then(r => r.json()).then(e =>
      [e.method, e.headers['content-type'] ?? null, e.body]);
    Promise.all([
      echo({ method: 'post', body: 'café' }),
      echo({ method: 'PUT', body: new URLSearchParams({ a: '1 2', b: '&' }) }),
      echo({ method: 'POST', body: new Uint8Array([65, 66]) }),
      echo({ method: 'POST', body: new Uint8Array([1, 65, 66, 2]).subarray(1, 3).buffer.slice(1, 3) }),
      echo({ method: 'POST', body: JSON.stringify({ x: 1 }), headers: { 'Content-Type': 'application/json' } }),
      echo({ method: 'DELETE' }),
      echo({ method: 'PATCH', body: 'p' }),
    ]).then(results => { out = results; }))"),
            R"([["POST","text/plain;charset=UTF-8","café"],)"
            R"(["PUT","application/x-www-form-urlencoded;charset=UTF-8","a=1+2&b=%26"],)"
            R"(["POST","application/octet-stream","AB"],["POST","application/octet-stream","AB"],)"
            R"(["POST","application/json","{\"x\":1}"],["DELETE",null,""],["PATCH","text/plain;charset=UTF-8","p"]])");
}

// The headers that only the browser sets are ignored; Accept and Referer are set as browsers do.
TEST(WebNetworkTest, FetchHeaders) {
  WebClient client(Config());
  const std::string out = Out(client, R"(fetch('/api#fragment', { headers: [['X-Test', ' a '], ['x-test', 'b'],
      ['Cookie', 'forged=1'], ['Host', 'evil.example'], ['Origin', 'http://evil.example'], ['Sec-Fetch-Mode', 'x'],
      ['Proxy-Authorization', 'x'], ['X-HTTP-Method-Override', 'TRACE'], ['X-Method-Override', 'PATCH']] })
    .then(r => r.json()).then(e => { out = e.headers; }))");
  EXPECT_TRUE(out.contains(R"("x-test":"a, b")")) << out;
  EXPECT_TRUE(out.contains(R"("accept":"*/*")")) << out;
  EXPECT_TRUE(out.contains(R"("x-method-override":"PATCH")")) << out;
  EXPECT_TRUE(out.contains(R"("referer":")" + Url("/html?src=")));  // the document, without fragment
  EXPECT_TRUE(out.contains(R"("host":"127.0.0.1:)")) << out;
  for (const std::string_view forbidden : {R"("cookie":)", R"("origin":)", R"("sec-fetch-mode":)",
                                           R"("proxy-authorization":)", R"("x-http-method-override":)"}) {
    EXPECT_FALSE(out.contains(forbidden)) << forbidden << " in " << out;
  }
  // An Accept of the script replaces the default one.
  EXPECT_EQ(Out(client, R"(fetch('/api', { headers: { accept: 'text/html' } }).then(r => r.json())
    .then(e => { out = e.headers.accept; }))"),
            R"("text/html")");
  // Same origin: the whole URL of the document. Another origin: its origin only.
  EXPECT_EQ(Out(client, "fetch('" + SameSiteUrl("/api?acao=*") + R"(').then(r => r.json())
    .then(e => { out = e.headers.referer; }))"),
            "\"" + Url("/") + "\"");
}

TEST(WebNetworkTest, FetchErrors) {
  WebClient client(Config());
  test::TestServer closed = test::MakePageServer();
  const std::string closedUrl = test::ServerUrl(closed) + "/api";
  closed.stop();
  EXPECT_EQ(
      Out(client, R"(const outcome = promise => promise.then(() => 'resolved', e => e.name + ': ' + e.message);
    Promise.all([
      outcome(fetch(')" +
                      closedUrl + R"(')),
      outcome(fetch('http://[bad')),
      outcome(fetch('data:text/plain,x')),
      outcome(fetch('/api', { method: 'GET', body: 'x' })),
      outcome(fetch('/api', { method: 'TRACE' })),
      outcome(fetch('/api', { method: 'patch' })),
      outcome(fetch('/api', { method: 'bad method' })),
      outcome(fetch('/api', { mode: 'navigate' })),
      outcome(fetch('/api', { credentials: 'all' })),
      outcome(fetch('/api', { headers: { 'bad name': 'x' } })),
      outcome(fetch('/api', { headers: { 'x-a': 'a\nb' } })),
      outcome(fetch('/api', { headers: { 'x-a': 'café' } })),
      outcome(fetch('/api', { method: 'POST', body: 'x', headers: { 'content-type': 'x' } })),
      outcome(fetch(')" +
                      closedUrl + R"(', { method: 'PUT' })),
    ]).then(results => { out = results; }))"),
      R"(["TypeError: Failed to fetch","TypeError: Failed to construct 'Request': Invalid URL 'http://[bad'",)"
      R"("TypeError: aeronet: only http and https URLs can be fetched, not 'data:text/plain,x'",)"
      R"("TypeError: Failed to construct 'Request': Request with GET/HEAD method cannot have body.",)"
      R"("TypeError: 'TRACE' HTTP method is unsupported",)"
      R"("TypeError: aeronet: the HTTP method 'patch' is not supported",)"
      R"("TypeError: 'bad method' is not a valid HTTP method","TypeError: 'navigate' is not a valid request mode",)"
      R"("TypeError: 'all' is not a valid credentials mode","TypeError: 'bad name' is not a valid HTTP header name",)"
      R"("TypeError: 'a\nb' is not a valid HTTP header value","TypeError: aeronet: invalid header 'x-a: café'",)"
      R"("TypeError: aeronet: unsupported Content-Type 'x'",)"
      R"("TypeError: the CORS preflight of the request to another origin failed"])");
}

TEST(WebNetworkTest, FetchFollowsRedirects) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(fetch('/redirect?to=' + encodeURIComponent('/api?body=moved')).then(async r => {
    out = [r.redirected, r.url.endsWith('/api?body=moved'), await r.text()];
  }))"),
            R"([true,true,"moved"])");
  // A redirect status without Location is a response.
  EXPECT_EQ(Out(client, R"(fetch('/api?status=302&body=stays').then(async r => {
    out = [r.status, r.redirected, await r.text()];
  }))"),
            R"([302,false,"stays"])");
}

// With HttpClientConfig::followRedirects off, the scripts get the redirects.
TEST(WebNetworkTest, RedirectsNotFollowedByTheClient) {
  WebClientConfig config = Config();
  config.http.followRedirects = false;
  WebClient client(config);
  EXPECT_EQ(Out(client, R"(fetch('/redirect?to=' + encodeURIComponent('/api?body=moved')).then(async r => {
    out = [r.status, r.redirected, r.headers.get('location')];
  }))"),
            R"([302,false,"/api?body=moved"])");
}

// HEAD requests need no preflight, and send no Origin to their own origin.
TEST(WebNetworkTest, HeadRequests) {
  WebClient client(Config());
  const int preflights = gPreflights.load();
  EXPECT_EQ(Out(client, "const other = '" + SameSiteUrl("/api?acao=*") + R"(';
    Promise.all([fetch('/api', { method: 'HEAD' }), fetch(other, { method: 'HEAD' })]).then(async ([own, cross]) => {
      out = [own.status, await own.text(), cross.status, cross.type];
    }))"),
            R"([200,"",200,"cors"])");
  EXPECT_EQ(gPreflights.load(), preflights);
}

TEST(WebNetworkTest, AbortController) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(const controller = new AbortController();
    const aborted = fetch('/api', { signal: controller.signal }).catch(e => e.name);
    controller.abort();
    const reason = fetch('/api', { signal: AbortSignal.abort('why') }).catch(e => e);
    const any = AbortSignal.any([new AbortController().signal, AbortSignal.abort(1)]);
    let events = 0;
    const later = new AbortController();
    later.signal.onabort = () => ++events;
    later.signal.addEventListener('abort', () => ++events);
    const sent = fetch('/api?body=ok', { signal: later.signal }).then(r => r.text());
    Promise.all([aborted, reason, sent]).then(results => {
      later.abort();
      later.abort();
      out = [...results, any.aborted, any.reason, events, later.signal.reason.name, controller.signal.aborted,
        typeof AbortSignal.timeout(10).aborted];
      try { new AbortSignal(); } catch (e) { out.push(e.message); }
      try { AbortSignal.abort('x').throwIfAborted(); } catch (e) { out.push(e); }
    }))"),
            R"(["AbortError","why","ok",true,1,2,"AbortError",true,"boolean","Illegal constructor","x"])");
}

TEST(WebNetworkTest, HeadersApi) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(const h = new Headers({ B: '2', a: '1' });
    h.append('b', '3');
    h.append('Set-Cookie', 'x=1');
    h.append('set-cookie', 'y=2');
    out = [[...h], h.get('B'), h.get('missing'), h.has('A'), [...h.keys()], [...h.values()], h.getSetCookie()];
    h.set('b', '4');
    h.delete('set-cookie');
    const each = [];
    h.forEach((value, name) => each.push(name + '=' + value));
    out.push(each, [...new Headers(h).entries()], [...new Headers([['c', ' 5 ']])]);
    for (const bad of [() => new Headers(1), () => new Headers([['a']]), () => h.append('a b', 'x')]) {
      try { bad(); } catch (e) { out.push(e.name); }
    })"),
            R"([[["a","1"],["b","2, 3"],["set-cookie","x=1"],["set-cookie","y=2"]],"2, 3",null,true,)"
            R"(["a","b","set-cookie","set-cookie"],["1","2, 3","x=1","y=2"],["x=1","y=2"],["a=1","b=4"],)"
            R"([["a","1"],["b","4"]],[["c","5"]],"TypeError","TypeError","TypeError"])");
}

TEST(WebNetworkTest, ResponseApi) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"((async () => {
    const created = new Response('body', { status: 201, statusText: 'Made', headers: { 'X-A': '1' } });
    const clone = created.clone();
    const json = Response.json({ a: 1 });
    const error = Response.error();
    const redirect = Response.redirect('/next', 301);
    const fetched = await fetch('/api?body=once');
    const fetchedClone = fetched.clone();
    out = [created.status, created.statusText, created.headers.get('content-type'), await created.text(),
      await clone.text(), json.headers.get('content-type'), await json.json(), error.type, error.status,
      redirect.status, redirect.headers.get('location').endsWith('/next'), new Response().body,
      await new Response(new Uint8Array([104, 105])).text(), await new Response(null).text(),
      await fetchedClone.text(), await fetched.text(), fetchedClone.type];
    for (const bad of [() => new Response('x', { status: 99 }), () => new Response('x', { status: 204 }),
      () => Response.redirect('/x', 200), () => Response.json(undefined), () => created.clone(),
      () => fetched.headers.set('a', 'b'), () => Response.redirect('http://[bad')]) {
      try { bad(); } catch (e) { out.push(e.name); }
    }
    out.push(await created.text().catch(e => e.name));
  })())"),
            R"([201,"Made","text/plain;charset=UTF-8","body","body","application/json",{"a":1},"error",0,301,)"
            R"(true,null,"hi","","once","once","basic","RangeError","TypeError","RangeError","TypeError",)"
            R"("TypeError","TypeError","TypeError","TypeError"])");
}

TEST(WebNetworkTest, RequestApi) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"((async () => {
    const request = new Request('/api?q', { method: 'post', body: 'b', headers: { 'X-A': '1' }, mode: 'same-origin',
      credentials: 'omit', cache: 'no-store', keepalive: true });
    const copy = new Request(request, { method: 'PUT' });
    const clone = request.clone();
    out = [request.url.endsWith('/api?q'), request.method, request.headers.get('x-a'), request.mode,
      request.credentials, request.cache, request.keepalive, request.redirect, request.integrity, request.referrer,
      request.signal.aborted, copy.method, await copy.text(), await clone.text(), await request.text(),
      new Request('/x').method, new Request('/x').mode, new Request('/x').credentials, request.destination,
      request.referrerPolicy, request.duplex];
    const echoed = await (await fetch(new Request('/api', { method: 'POST', body: 'from request' }))).json();
    out.push(echoed.body);
  })())"),
            R"([true,"POST","1","same-origin","omit","no-store",true,"follow","","about:client",false,"PUT","b","b",)"
            R"("b","GET","cors","same-origin","","","half","from request"])");
}

// The network and text APIs are set up on the first lookup of one of their globals, inherited by the window: a script
// can replace one before, by its own property.
TEST(WebNetworkTest, NetworkGlobalsAreSetUpOnFirstUse) {
  WebClient client(Config());
  for (int page = 0; page < 2; ++page) {  // the first page of the client compiles them, the next one reads them back
    EXPECT_EQ(Out(client, R"(const own = name => Object.hasOwn(window, name);
      const before = ['fetch' in window, 'Headers' in window, own('URL')];
      window.fetch = () => 'replaced';
      out = [...before, own('fetch'), fetch(), typeof URL, own('URL'), typeof Headers, 'nope' in window,
        URL.toString().includes('[native code]'), typeof navigator.sendBeacon];)"),
              R"([true,true,false,true,"replaced","function",false,"function",false,true,"function"])");
  }
}

// --- The page loop ---

// The responses are tasks between the others: after the timers due at once, in the order of the requests.
#if AERONET_WANT_MALLOC_OVERRIDES
// The network APIs are compiled on their first use in a client: a compilation that fails (memory exhaustion of the
// process, the prelude is compiled without the limits of the page) is an error of the script, and the next use
// compiles them again.
TEST(WebNetworkTest, FailedCompilationOfTheNetworkApis) {
  WebClient client(Config());
  WebPage page = RunScript(client, "");
  test::FailReallocsOfAtLeast(1024);
  const auto failed = page.evaluate("typeof fetch");
  test::ResetToSysRealloc();
  EXPECT_FALSE(failed);
  EXPECT_EQ(test::Eval(page, "typeof fetch"), R"("function")");
}
#endif

TEST(WebNetworkTest, ResponsesAreTasks) {
  WebClient client(Config());
  WebPage page = test::Navigate(client, test::PageUrl(gServer, R"(<script>log = [];
      fetch('/api?body=1').then(r => r.text()).then(t => log.push('fetch ' + t + ' ' + document.readyState));
      setTimeout(() => log.push('timer'));
      fetch('/api?body=2').then(r => r.text()).then(t => log.push('fetch ' + t));
      log.push('script');
      document.addEventListener('DOMContentLoaded', () => log.push('DOMContentLoaded'));
      window.addEventListener('load', () => {
        log.push('load');
        fetch('/api?body=3').then(r => r.text()).then(t => log.push('after load ' + t));
      });</script>)"),
                                {.until = LoadState::Load});
  EXPECT_EQ(test::Eval(page, "log"), R"(["script","timer","fetch 1 loading","fetch 2","DOMContentLoaded","load",)"
                                     R"("after load 3"])");
}

// A single-page app that loads its data, then renders it: from a timer, after load.
TEST(WebNetworkTest, NetworkIdleWaitsForTheRequests) {
  WebClient client(Config());
  constexpr std::string_view kApp = R"(<div id=app></div><script>
    window.addEventListener('load', () => setTimeout(async () => {
      const rows = await (await fetch('/api?body=' + encodeURIComponent('["a","b"]'))).json();
      await new Promise(resolve => setTimeout(resolve, 500));
      const more = await (await fetch('/api?body=' + encodeURIComponent('["c"]'))).json();
      document.getElementById('app').innerHTML = [...rows, ...more].map(r => '<p class=row>' + r + '</p>').join('');
    }, 100));</script>)";
  WebPage loaded = test::Navigate(client, test::PageUrl(gServer, kApp));
  EXPECT_FALSE(loaded.document().querySelector("p.row"));
  WebPage idle = test::Navigate(client, test::PageUrl(gServer, kApp), {.until = LoadState::NetworkIdle});
  EXPECT_EQ(idle.document().querySelectorAll("p.row").size(), 3U);
  WebPage matched =
      test::Navigate(client, test::PageUrl(gServer, kApp), {.untilSelector = "p.row", .until = LoadState::NetworkIdle});
  EXPECT_EQ(matched.stopReason(), StopReason::SelectorMatched);
}

TEST(WebNetworkTest, MaxRequests) {
  WebClientConfig config = Config();
  config.maxRequests = 2;
  WebClient client(config);
  EXPECT_EQ(Out(client, R"(Promise.all([fetch('/api'), fetch('/api'), fetch('/api')].map(p => p.then(r => r.status,
    e => e.message))).then(results => {
      out = results;
      const xhr = new XMLHttpRequest();
      xhr.open('GET', '/api', false);
      try { xhr.send(); } catch (e) { out.push(e.name); }
      const async = new XMLHttpRequest();
      async.onerror = () => out.push('async ' + async.readyState);
      async.open('GET', '/api');
      async.send();
    }))"),
            R"([200,200,"aeronet: the scripts of the document exceed WebClientConfig::maxRequests","NetworkError",)"
            R"("async 4"])");
}

// Endless requests stop at the deadline of the navigation.
TEST(WebNetworkTest, EndlessRequestsStopAtTheDeadline) {
  WebClientConfig config = Config();
  config.maxRequests = std::numeric_limits<uint32_t>::max();
  WebClient client(config);
  WebPage page =
      RunScript(client, "count = 0; const next = () => fetch('/api?body=1').then(() => { ++count; next(); }); next()",
                {.until = LoadState::NetworkIdle, .timeout = milliseconds{300}});
  EXPECT_EQ(page.stopReason(), StopReason::TimedOut);
  EXPECT_GT(std::stoi(test::Eval(page, "count")), 0);
}

// Each step of the requests of a script fails in turn as the memory limit grows: no value leaks (QuickJS asserts that
// no object is left when a runtime is freed), and the script completes once the limit is enough. The setup of a page
// takes most of the memory: the limit grows fast until a page sets up, then by small steps.
TEST(WebNetworkTest, MemoryLimitDuringRequests) {
  static constexpr std::size_t kStep = 1024;
  const std::string padding = "//" + std::string(internal::ScriptCache::kMinScriptSize, 'p') + "\n";
  const std::string script = padding + R"(Promise.all([
      fetch('/api?body=' + 'x'.repeat(100), { headers: { 'X-A': '1' } }).then(r => r.text()),
      fetch('/redirect?to=' + encodeURIComponent('/api?body=caf\u00e9')).then(r => r.text()),
      fetch('/api', { method: 'POST', body: 'b\u00e9' }).then(r => r.json()),
      new Promise(resolve => {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/api');
        xhr.onload = () => resolve(xhr.getAllResponseHeaders().length > 0);
        xhr.send(new Uint8Array([1, 2]));
      }),
      String(new URL('/x?a=1', location.href).searchParams), new TextDecoder().decode(new TextEncoder().encode('t\u00e9')),
    ]).then(results => { out = results.length; }))";
  const std::string url = test::PageUrl(gServer, "<script>" + script + "</script>");
  const auto completes = [&url](std::size_t limit) {
    WebClientConfig config = Config();
    config.maxScriptMemory = limit;
    WebClient client(config);
    try {
      auto page = client.navigate(url);
      return page && page->evaluate("out").value_or("") == "6" ? 1 : 0;
    } catch (const std::bad_alloc&) {
    } catch (const std::runtime_error&) {  // too little memory for the setup of the page
    }
    return -1;
  };
  std::size_t limit = std::size_t{128} << 10U;
  for (std::size_t next = limit; completes(next) < 0; next += next / 16) {
    ASSERT_LT(next, std::size_t{64} << 20U);
    limit = next;
  }
  const std::size_t setupLimit = limit;
  int failures = 0;
  for (; completes(limit) <= 0; limit += kStep) {
    ASSERT_LT(limit, setupLimit + (std::size_t{1} << 20U)) << "the script never completes";
    ++failures;
  }
  EXPECT_GT(failures, 10);
}

// The native memory that the requests not sent yet hold is bounded too: their bodies fill the budget of the page.
TEST(WebNetworkTest, QueuedRequestsAreBounded) {
  WebClientConfig config = Config();
  config.maxScriptMemory = std::size_t{4} << 20U;
  config.maxRequests = std::numeric_limits<uint32_t>::max();
  WebClient client(config);
  WebPage page = RunScript(client, "");
  // evaluate() sends no request: they stay queued until the page dies. A request that the page cannot hold is a
  // rejection of fetch().
  EXPECT_EQ(test::Eval(page,
                       "rejected = 0; const big = 'x'.repeat(1 << 16); for (let i = 0; i < 1000; ++i) "
                       "fetch('/api', { method: 'POST', body: big }).catch(e => { rejected += "
                       "String(e).includes('out of memory'); }); 0"),
            "0");
  EXPECT_GT(std::stoi(test::Eval(page, "rejected")), 900);
}

TEST(WebNetworkTest, ExceptionsOfTheCallbacksAreReported) {
  WebClient client(Config());
  WebPage page = RunScript(client,
                           "fetch('/api').then(() => { throw new Error('in then'); });"
                           "const xhr = new XMLHttpRequest(); xhr.onload = () => { throw new Error('in onload'); };"
                           "xhr.open('GET', '/api'); xhr.send();");
  ASSERT_EQ(page.uncaughtErrors().size(), 2U);
  EXPECT_TRUE(page.uncaughtErrors()[0].contains("in then")) << page.uncaughtErrors()[0];
  EXPECT_TRUE(page.uncaughtErrors()[1].contains("in onload")) << page.uncaughtErrors()[1];
}

// A response larger than the free JavaScript heap is a network error for the script, which goes on: its signal can
// abort, its XMLHttpRequest be opened again.
TEST(WebNetworkTest, ResponsesBeyondTheMemoryOfTheScripts) {
  WebClientConfig config = Config();
  config.maxScriptMemory = std::size_t{4} << 20U;
  // Uncompressed responses: without zstd, the server picks brotli at its default quality (11), whose 6 MiB take seconds
  // in a Debug build, beyond the time limit of the navigation under a parallel ctest.
  config.http.withDefaultAcceptEncoding(http::identity);
  WebClient client(config);
  EXPECT_EQ(Out(client, R"(out = [];
    const controller = new AbortController();
    const large = '/large?size=' + (6 << 20);
    fetch(large, { signal: controller.signal }).then(() => out.push('loaded'), e => {
      out.push(e.name + ' ' + e.message);
      controller.abort();
      const xhr = new XMLHttpRequest();
      xhr.onerror = () => {
        out.push('error ' + xhr.readyState);
        xhr.onload = () => out.push('reopened ' + xhr.responseText);
        xhr.open('GET', '/api?body=ok');
        xhr.send();
      };
      xhr.open('GET', large);
      xhr.send();
    }))"),
            R"x(["TypeError the response does not fit in the memory of the scripts )x"
            R"x((WebClientConfig::maxScriptMemory)","error 4","reopened ok"])x");
}

// A request already sent is no longer cancelled, also when its callback could not forget it: its signal lost its
// removeEventListener(), or got one that throws (an exception of the response task, its promise left pending until
// the abort).
// A body that the memory of the scripts cannot hold as UTF-8 fails its request, not the page.
TEST(WebNetworkTest, BodyBeyondTheMemoryOfTheScripts) {
  WebClientConfig config = Config();
  config.maxScriptMemory = std::size_t{16} << 20U;
  WebClient client(config);
  const int counted = gCounted.load();
  EXPECT_EQ(Out(client, R"(let big = '\u20ac'.repeat(5 << 20);  // 10 MB as UTF-16, 15 MB as UTF-8
    fetch('/api?count=1', { method: 'POST', body: big }).then(() => 'sent', e => e.name).then(async outcome => {
      big = null;
      out = [outcome, await fetch('/api?body=ok').then(r => r.text())];
    });)"),
            R"(["InternalError","ok"])");
  EXPECT_EQ(gCounted.load(), counted);
}

TEST(WebNetworkTest, AbortAfterTheResponse) {
  WebClient client(Config());
  WebPage page = RunScript(client, R"(out = [];
    const throwing = new AbortController();
    throwing.signal.removeEventListener = () => { throw new Error('in removeEventListener'); };
    fetch('/api?body=lost', { signal: throwing.signal }).then(() => out.push('resolved'), e => out.push(e.name));
    const forgotten = new AbortController();
    forgotten.signal.removeEventListener = () => {};
    fetch('/api?body=ok', { signal: forgotten.signal }).then(r => r.text()).then(text => {
      forgotten.abort();
      throwing.abort();
      out.push(text, forgotten.signal.aborted, throwing.signal.aborted);
    });)");
  EXPECT_EQ(test::Eval(page, "out"), R"(["ok",true,true,"AbortError"])");
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].contains("in removeEventListener")) << page.uncaughtErrors()[0];
}

// --- XMLHttpRequest ---

TEST(WebNetworkTest, XmlHttpRequestEvents) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(out = [];
    const xhr = new XMLHttpRequest();
    out.push(xhr.readyState, XMLHttpRequest.DONE, xhr.DONE, xhr.status, xhr.responseText, xhr.getAllResponseHeaders());
    xhr.onreadystatechange = () => out.push('readystatechange ' + xhr.readyState);
    for (const type of ['loadstart', 'progress', 'load', 'loadend']) {
      xhr.addEventListener(type, e => out.push(type + ' ' + e.loaded + ' ' + e.lengthComputable));
    }
    xhr.open('POST', '/api?status=202&reason=Accepted&body=done&type=text/plain');
    xhr.setRequestHeader('X-A', '1');
    xhr.send('payload');
    xhr.onloadend = () => out.push(xhr.status, xhr.statusText, xhr.responseText, xhr.response,
      xhr.responseURL.endsWith('&body=done&type=text/plain'), xhr.getResponseHeader('X-CUSTOM'),
      xhr.getResponseHeader('missing'), xhr.getAllResponseHeaders().includes('x-custom: custom\r\n'),
      xhr.responseXML, xhr.upload instanceof XMLHttpRequestEventTarget);)"),
            R"([0,4,4,0,"","","readystatechange 1","loadstart 0 false","readystatechange 2","readystatechange 3",)"
            R"("progress 4 true","readystatechange 4","load 4 true","loadend 4 true",202,"Accepted","done","done",)"
            R"(true,"custom",null,true,null,true])");
}

TEST(WebNetworkTest, XmlHttpRequestResponseTypes) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(out = [];
    const get = (type, body, check) => {
      const xhr = new XMLHttpRequest();
      xhr.open('GET', '/api?body=' + encodeURIComponent(body));
      xhr.responseType = type;
      xhr.onload = () => out.push(check(xhr));
      xhr.send();
    };
    get('json', '{"a":1}', xhr => xhr.response);
    get('json', 'not json', xhr => xhr.response);
    get('arraybuffer', 'ab', xhr => xhr.response.byteLength);
    get('text', 'text', xhr => xhr.response);
    get('blob', 'b', xhr => xhr.response);
    get('json', 'x', xhr => { try { return xhr.responseText; } catch (e) { return e.name; } });
    const xhr = new XMLHttpRequest();
    xhr.responseType = 'unknown';
    out.push(xhr.responseType);
    xhr.open('GET', '/api?body=' + encodeURIComponent('café'));
    xhr.overrideMimeType('text/plain; charset=windows-1252');
    xhr.onload = () => out.push(xhr.responseText);
    xhr.send();)"),
            R"(["",{"a":1},null,2,"text",null,"InvalidStateError","cafÃ©"])");
}

TEST(WebNetworkTest, SynchronousXmlHttpRequest) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(out = [];
    const xhr = new XMLHttpRequest();
    xhr.onreadystatechange = () => out.push('readystatechange ' + xhr.readyState);
    xhr.onload = () => out.push('load');
    xhr.open('PUT', '/api', false);
    xhr.send(new URLSearchParams('a=1'));
    out.push(JSON.parse(xhr.responseText).body, xhr.status);
    const failed = new XMLHttpRequest();
    failed.open('GET', 'http://127.0.0.1:1/', false);
    try { failed.send(); } catch (e) { out.push(e.name, failed.readyState, failed.status); })"),
            R"(["readystatechange 1","readystatechange 4","load","a=1",200,"NetworkError",4,0])");
}

TEST(WebNetworkTest, XmlHttpRequestErrorsAndAbort) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(out = [];
    const failed = new XMLHttpRequest();
    failed.onerror = e => out.push('error ' + e.type + ' ' + failed.readyState + ' ' + failed.status);
    failed.open('GET', 'http://127.0.0.1:1/');
    failed.send();
    const aborted = new XMLHttpRequest();
    aborted.onabort = () => out.push('abort ' + aborted.readyState);
    aborted.onload = () => out.push('aborted request loaded');
    aborted.open('GET', '/api');
    aborted.send();
    aborted.abort();
    out.push('after abort ' + aborted.readyState);
    const reopened = new XMLHttpRequest();
    reopened.onload = () => out.push('reopened ' + reopened.responseText);
    reopened.open('GET', '/api?body=first');
    reopened.send();
    reopened.open('GET', '/api?body=second');
    reopened.send();
    const fresh = new XMLHttpRequest();
    const errorOf = action => { try { action(); return 'no error'; } catch (e) { return e.name; } };
    out.push(errorOf(() => fresh.setRequestHeader('a', 'b')), errorOf(() => fresh.send()),
      errorOf(() => fresh.open('GET', 'http://[bad')), errorOf(() => fresh.open('TRACK', '/')));
    fresh.open('GET', '/api');
    fresh.send();
    out.push(errorOf(() => fresh.send()), errorOf(() => { fresh.withCredentials = true; }));
    fresh.abort();
    fresh.abort();
    out.push(fresh.readyState);
    const abortInLoadStart = new XMLHttpRequest();
    abortInLoadStart.onload = () => out.push('abortInLoadStart loaded');
    abortInLoadStart.onabort = () => out.push('abortInLoadStart aborted');
    abortInLoadStart.onloadstart = () => abortInLoadStart.abort();
    abortInLoadStart.open('GET', '/api?body=lost');
    abortInLoadStart.send();
    out.push('after abortInLoadStart ' + abortInLoadStart.readyState);
    const reopenInLoadStart = new XMLHttpRequest();
    reopenInLoadStart.onload = () => out.push('reopenInLoadStart ' + reopenInLoadStart.responseText);
    reopenInLoadStart.onloadstart = () => {
      reopenInLoadStart.onloadstart = null;
      reopenInLoadStart.open('GET', '/api?body=new');
      reopenInLoadStart.send();
    };
    reopenInLoadStart.open('GET', '/api?body=old');
    reopenInLoadStart.send();)"),
            R"(["abort 4","after abort 0","InvalidStateError","InvalidStateError","SyntaxError","SecurityError",)"
            R"("InvalidStateError","InvalidStateError",0,"abortInLoadStart aborted","after abortInLoadStart 0",)"
            R"("error error 4 0","reopened second","reopenInLoadStart new"])");
}

// A request that XMLHttpRequest.send() cannot make is a network error, reported as a task after send() returns, unless
// abort() or open() comes first.
TEST(WebNetworkTest, XmlHttpRequestRequestsThatCannotBeMade) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(out = [];
    const ftp = new XMLHttpRequest();
    for (const type of ['readystatechange', 'loadstart', 'error', 'abort', 'loadend']) {
      ftp.addEventListener(type, () => out.push(type + ' ' + ftp.readyState));
    }
    ftp.open('GET', 'ftp://example.com/');
    ftp.send();
    out.push('sent');
    const aborted = new XMLHttpRequest();
    aborted.onerror = () => out.push('aborted request failed');
    aborted.onabort = () => out.push('aborted');
    aborted.open('GET', 'ftp://example.com/');
    aborted.send();
    aborted.abort();
    const reopened = new XMLHttpRequest();
    reopened.onerror = () => out.push('reopened request failed');
    reopened.onload = () => out.push('reopened ' + reopened.responseText);
    reopened.open('GET', 'ftp://example.com/');
    reopened.send();
    reopened.open('GET', '/api?body=second');
    reopened.send();)"),
            R"(["readystatechange 1","loadstart 1","sent","aborted","readystatechange 4","error 4","loadend 4",)"
            R"("reopened second"])");
}

TEST(WebNetworkTest, SendBeacon) {
  WebClient client(Config());
  const int before = gBeacons.load();
  EXPECT_EQ(Out(client, R"(out = [navigator.sendBeacon('/api?beacon=1', 'data'), navigator.sendBeacon('/api?beacon=1'),
    navigator.sendBeacon('http://[bad')];)"),
            "[true,true,true]");
  EXPECT_EQ(gBeacons.load() - before, 2);
}

// --- CORS ---

TEST(WebNetworkTest, ResponsesOfAnotherOriginNeedTheConsentOfTheirServer) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, "const api = '" + SameSiteUrl("/api?body=x") + R"(';
    const typeOf = (url, init) => fetch(url, init).then(r => r.type + ' ' + r.status, e => e.name);
    Promise.all([typeOf(api), typeOf(api + '&acao=*'), typeOf(api + '&acao=origin'),
      typeOf(api + '&acao=http://other.example'), typeOf(api + '&acao=*', { credentials: 'include' }),
      typeOf(api + '&acao=origin&acac=true', { credentials: 'include' }),
      typeOf(api + '&acao=origin', { credentials: 'include' }), typeOf(api + '&acao=*', { mode: 'same-origin' }),
      typeOf('/api', { mode: 'no-cors' }), typeOf('/api', { mode: 'same-origin' }),
      typeOf('/redirect?to=' + encodeURIComponent(api + '&acao=*'), { mode: 'same-origin' }),
      typeOf('/redirect?to=' + encodeURIComponent(api + '&acao=*')),
      typeOf('/redirect?to=' + encodeURIComponent(api)),
    ]).then(results => { out = results; }))"),
            R"(["TypeError","cors 200","cors 200","TypeError","TypeError","cors 200","TypeError","TypeError",)"
            R"("basic 200","basic 200","TypeError","cors 200","TypeError"])");
}

// Each redirect of a request of a script follows the rules of the Fetch Standard: the same-origin mode fails before a
// request to another origin, the redirect of another origin needs the consent of its server, and the Origin and the
// Referer are those of the new URL, the Origin "null" after a redirect from another origin to a third one.
TEST(WebNetworkTest, RedirectsOfTheScripts) {
  gCounted = 0;
  WebClient client(Config());
  const std::string origin = test::ServerUrl(gServer);
  EXPECT_EQ(Out(client, "const other = '" + SameSiteUrl("") + "'; const third = '" + CrossSiteUrl("") +
                            "'; const origin = '" + origin + R"(';
    const enc = encodeURIComponent;
    const statusOf = (url, init) => fetch(url, init).then(r => r.status, e => e.message);
    const headersOf = url => fetch(url).then(r => r.json()).then(r => [r.headers.origin, r.headers.referer]);
    Promise.all([
      statusOf('/redirect?to=' + enc(other + '/api?count=1&acao=*'), { mode: 'same-origin' }),
      headersOf('/redirect?to=' + enc(other + '/api?acao=*')),
      statusOf(other + '/redirect?to=' + enc(other + '/api?acao=*')),
      headersOf(other + '/redirect?acao=*&to=' + enc(third + '/api?acao=origin')),
      statusOf(other + '/redirect?acao=*&to=' + enc(third + '/api?acao=' + enc(origin))),
      headersOf(other + '/redirect?acao=*&to=' + enc(third + '/redirect?acao=*&to=' + enc(third + '/api?acao=*'))),
    ]).then(results => { out = results; }))"),
            R"(["redirected to another origin in the same-origin mode",[")" + origin + R"(",")" + origin +
                R"x(/"],"the redirect of another origin does not allow the document to follow it (CORS)",)x"
                R"(["null",")" +
                origin + R"x(/"],"the response of another origin does not allow the document to read it (CORS)",)x" +
                R"(["null",")" + origin + R"(/"]])");
  EXPECT_EQ(gCounted, 0);
}

// A redirect that needs a preflight at its new URL gets one (with the method that the redirect gives the request), and
// a preflight is never redirected.
TEST(WebNetworkTest, PreflightsOfTheRedirects) {
  gPreflights = 0;
  gCounted = 0;
  WebClient client(Config());
  EXPECT_EQ(Out(client, "const other = '" + SameSiteUrl("") + "'; const third = '" + CrossSiteUrl("") + R"(';
    const enc = encodeURIComponent;
    const put = url => fetch(url, { method: 'PUT' }).then(r => r.json()).then(r => r.method, e => e.message);
    Promise.all([
      put('/api?to=' + enc(other + '/api?acao=origin&acam=PUT')),
      put('/api?to=' + enc(other + '/api?count=1&acao=origin')),
      put(other + '/redirect?status=307&to=' + enc(other + '/api?acao=origin&acam=PUT')),
      put(other + '/api?acao=origin&acam=PUT&redirect=303&to=' + enc(other + '/api?acao=origin')),
      put('/api?to=' + enc(third + '/api?acao=origin&acam=PUT')),
    ]).then(results => { out = results; }))"),
            R"(["PUT","the CORS preflight of the request to another origin failed",)"
            R"("the CORS preflight of the request to another origin failed","GET","PUT"])");
  // The preflights of /api of the first two requests and of the last one (its chain continued after a redirect to
  // another site), and that of the fourth one at its first URL: the GET of its 303 needs none.
  EXPECT_EQ(gPreflights, 4);
  EXPECT_EQ(gCounted, 0);
}

// The redirects of a request of a script are bounded by the maxRedirects of the HTTP client, also along the
// preflights: beyond them, a network error.
TEST(WebNetworkTest, RedirectsAreBounded) {
  WebClientConfig config = Config();
  config.http.maxRedirects = 1;
  WebClient client(config);
  EXPECT_EQ(
      Out(client, "const other = '" + SameSiteUrl("") + R"(';
    const enc = encodeURIComponent;
    const statusOf = (url, init) => fetch(url, init).then(r => r.status, e => e.message);
    Promise.all([
      statusOf('/redirect?to=' + enc('/api?body=x')),
      statusOf('/redirect?to=' + enc('/redirect?to=' + enc('/api?body=x'))),
      statusOf('/api?to=' + enc(other + '/api?acao=origin&acam=PUT&to=' + enc(other + '/api?acao=origin&acam=PUT')),
        { method: 'PUT' }),
    ]).then(results => { out = results; }))"),
      R"x([200,"the redirect was not followed (too many redirects, or from https to http)","too many redirects"])x");
}

TEST(WebNetworkTest, OpaqueResponses) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, "const api = '" + SameSiteUrl("/api?body=secret") + R"(';
    const errorOf = init => fetch(api, init).then(() => 'resolved', e => e.message);
    fetch(api, { mode: 'no-cors', method: 'POST', body: 'b' }).then(async r => {
      out = [r.type, r.status, r.ok, r.statusText, r.url, [...r.headers], await r.text()];
      out.push(await errorOf({ mode: 'no-cors', method: 'PUT' }),
        await errorOf({ mode: 'no-cors', headers: { 'X-A': '1' } }));
    }))"),
            R"(["opaque",0,false,"","",[],"","aeronet: the method 'PUT' is not allowed in the no-cors mode",)"
            R"("aeronet: a no-cors request only takes CORS-safelisted headers, not 'x-a'"])");
}

// A response of another origin shows its safelisted headers, and those it exposes. Set-Cookie never shows.
TEST(WebNetworkTest, ExposedHeaders) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, "const api = '" + SameSiteUrl("/api?body=x&cookie=c%3D1") + R"(';
    const names = (url, init) => fetch(url, init).then(r => [...r.headers.keys()].join(' '));
    Promise.all([names(api + '&acao=*'), names(api + '&acao=*&aceh=X-Custom'), names(api + '&acao=*&aceh=*'),
      names(api + '&acao=origin&acac=true&aceh=*', { credentials: 'include' }),
      names('/api?body=x&cookie=c%3D1'),
    ]).then(results => { out = results; }))"),
            R"(["content-length content-type","content-length content-type x-custom",)"
            R"("access-control-allow-origin access-control-expose-headers content-length content-type date server )"
            R"(x-custom","content-length content-type","content-length content-type date server x-custom"])");
}

// The requests that a form could not send need a preflight, that the server answers.
TEST(WebNetworkTest, Preflights) {
  WebClient client(Config());
  const int before = gPreflights.load();
  EXPECT_EQ(Out(client, "const api = '" + SameSiteUrl("/api?body=x&acao=origin") + R"(';
    const statusOf = (url, init) => fetch(url, init).then(r => r.status, e => e.name);
    Promise.all([
      statusOf(api, { method: 'PUT' }), statusOf(api + '&acam=PUT', { method: 'PUT' }),
      statusOf(api + '&acam=*', { method: 'DELETE' }),
      statusOf(api + '&acam=*&acac=true', { method: 'DELETE', credentials: 'include' }),
      statusOf(api, { headers: { 'X-B': '1', 'X-A': '2' } }), statusOf(api + '&acah=x-a,X-B', { headers: { 'X-B': '1', 'X-A': '2' } }),
      statusOf(api + '&acah=*', { headers: { 'X-A': '1' } }), statusOf(api + '&acah=*', { headers: { authorization: 'x' } }),
      statusOf(api, { method: 'POST', body: '{}', headers: { 'content-type': 'application/json' } }),
      statusOf(api + '&acah=content-type', { method: 'POST', body: '{}', headers: { 'content-type': 'application/json' } }),
      statusOf(api, { method: 'POST', body: 'x', headers: { 'content-type': 'text/plain' } }),
      statusOf(api, { headers: { accept: 'x'.repeat(200) } }), statusOf(api + '&acah=accept', { headers: { accept: 'x'.repeat(200) } }),
      statusOf(api, { headers: [['accept', 'a'.repeat(100)], ['accept', 'b'.repeat(100)]] }),
    ]).then(results => { out = results; }))"),
            R"(["TypeError",200,200,"TypeError","TypeError",200,200,"TypeError","TypeError",200,200,"TypeError",200,)"
            R"("TypeError"])");
  EXPECT_EQ(gPreflights.load() - before, 13);
}

TEST(WebNetworkTest, PreflightNamesTheUnsafeHeaders) {
  WebClient client(Config());
  EXPECT_EQ(
      Out(client, "fetch('" + SameSiteUrl("/api?body=x&acao=*&acah=*") +
                      R"(', { headers: { 'X-B': '1', 'Accept': 'text/html', 'X-A': '2', 'content-type': 'text/x' } })
    .then(r => { out = r.status; }))"),
      "200");
  EXPECT_EQ(LastPreflightHeaders(), "content-type,x-a,x-b");
}

// --- Credentials and cookies ---

TEST(WebNetworkTest, CredentialsModes) {
  WebClient client(Config());
  CookieJar& jar = client.httpClient().cookieJar();
  ASSERT_TRUE(jar.setCookie(Url("/"), "own=1; Path=/"));
  EXPECT_EQ(Out(client, "const other = '" + SameSiteUrl("/api?acao=origin&acac=true") + "'; const star = '" +
                            SameSiteUrl("/api?acao=*") + R"(';
    const cookies = (url, init) => fetch(url, init).then(r => r.json()).then(e => e.headers.cookie ?? '');
    Promise.all([cookies('/api'), cookies('/api', { credentials: 'omit' }), cookies('/api', { credentials: 'include' }),
      cookies(other), cookies(other, { credentials: 'include' }), cookies(other, { credentials: 'omit' }),
      cookies('/redirect?to=' + encodeURIComponent(star)),
      fetch('/api?cookie=' + encodeURIComponent('kept=1; Path=/')),
      fetch('/api?cookie=' + encodeURIComponent('omitted=1; Path=/'), { credentials: 'omit' }),
      fetch(other + '&cookie=' + encodeURIComponent('otherOrigin=1; Path=/')),
    ]).then(results => { out = [...results.slice(0, 7), document.cookie]; }))"),
            R"(["own=1","","own=1","","own=1","","","own=1; kept=1"])");
  // The requests of another origin with credentials store their cookies (cookies ignore the port).
  EXPECT_EQ(jar.cookieHeader(Url("/")), "own=1; kept=1");
  EXPECT_EQ(Out(client, "fetch('" + SameSiteUrl("/api?acao=origin&acac=true&cookie=x%3D1") +
                            R"(', { credentials: 'include' }).then(() => { out = document.cookie; }))"),
            R"("own=1; kept=1; x=1")");
}

// The requests of a document to another site are cross-site for its SameSite cookies, whatever their method.
TEST(WebNetworkTest, SameSiteCookiesOfTheSubresources) {
  WebClient client(Config());
  CookieJar& jar = client.httpClient().cookieJar();
  for (const std::string_view cookie :
       {"strict=1; SameSite=Strict", "lax=1; SameSite=Lax", "none=1; SameSite=None", "plain=1"}) {
    ASSERT_TRUE(jar.setCookie(CrossSiteUrl("/"), cookie));
    ASSERT_TRUE(jar.setCookie(Url("/"), cookie));
  }
  const std::string crossSiteApi = CrossSiteUrl("/api?acao=origin&acac=true");
  WebPage page = test::Navigate(
      client, test::PageUrl(gServer, "<script src='" + CrossSiteUrl("/cookies.js") +
                                         "'></script><script>"
                                         "const own = window.scriptCookies; const ownReferer = window.scriptReferer;"
                                         "</script><script src='" +
                                         Url("/cookies.js") +
                                         "'></script>"
                                         "<script>const same = window.scriptCookies;</script>"
                                         "<script src='" +
                                         CrossSiteUrl("/cookies.js") +
                                         "' crossorigin></script><script>"
                                         "const anonymous = window.scriptCookies;</script>"
                                         "<script src='" +
                                         CrossSiteUrl("/cookies.js") +
                                         "' crossorigin=Use-Credentials></script><script>"
                                         "const withCredentials = window.scriptCookies;"
                                         "const cookies = init => fetch('" +
                                         crossSiteApi +
                                         "', { credentials: 'include', ...init })"
                                         ".then(r => r.json()).then(e => e.headers.cookie ?? '');"
                                         "Promise.all([cookies(), cookies({ method: 'POST', body: 'x' }),"
                                         "fetch('" +
                                         crossSiteApi +
                                         "&cookie=' + encodeURIComponent('xlax=1; SameSite=Lax'), "
                                         "{ credentials: 'include' }),"
                                         "fetch('" +
                                         crossSiteApi +
                                         "&cookie=' + encodeURIComponent('xnone=1; SameSite=None'), "
                                         "{ credentials: 'include' })])"
                                         ".then(results => { out = [own, ownReferer, same, anonymous, "
                                         "withCredentials, results[0], results[1]]; });</script>"));
  EXPECT_EQ(test::Eval(page, "out"),
            R"(["none=1; plain=1",")" + Url("/") +
                R"(","strict=1; lax=1; none=1; plain=1","","none=1; plain=1","none=1; plain=1","none=1; plain=1"])");
  // A subresource of another site sets no SameSite=Strict or Lax cookie.
  EXPECT_EQ(jar.cookieHeader(CrossSiteUrl("/")), "strict=1; lax=1; none=1; plain=1; xnone=1");
}

#ifdef AERONET_ENABLE_OPENSSL
// A document of https sends no Referer to http (strict-origin-when-cross-origin): its scripts, its requests.
TEST(WebNetworkTest, NoRefererFromHttpsToHttp) {
  test::TlsTestServer tls;
  tls.router().setPath(http::Method::GET, "/html", [](const HttpRequestView& req) {
    return req.makeResponse(req.queryParamValue("src").value_or(""), http::ContentTypeTextHtml);
  });
  WebClientConfig config = Config();
  config.http.tlsVerifyPeer = false;  // self-signed certificate
  WebClient client(config);
  const std::string html = "<script src='" + Url("/cookies.js") + "'></script><script>fetch('" + Url("/api?acao=*") +
                           "').then(r => r.json()).then(e => { out = [window.scriptReferer, e.headers.referer ?? "
                           "null]; })</script>";
  WebPage page = test::Navigate(
      client, "https://127.0.0.1:" + std::to_string(tls.port()) + "/html?src=" + test::PercentEncode(html));
  EXPECT_EQ(test::Eval(page, "out"), R"(["",null])");
}
#endif

// --- Script cache ---

TEST(WebNetworkTest, ScriptCacheEntries) {
  using Script = internal::ScriptCache::Script;
  internal::ScriptCache cache(100);
  EXPECT_FALSE(cache.caches(internal::ScriptCache::kMinScriptSize - 1));
  EXPECT_TRUE(cache.caches(internal::ScriptCache::kMinScriptSize));
  EXPECT_FALSE(internal::ScriptCache(0).caches(1U << 20U));
  const auto bytes = [](std::size_t size) { return std::string(size, 'b'); };
  const auto span = [](const std::string& str) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(str.data()), str.size());
  };
  const std::string forty = bytes(40);
  const std::string fifty = bytes(50);
  const std::string thirty = bytes(30);
  cache.store(Script("u", "code 1"), span(forty));
  EXPECT_EQ(cache.find(Script("u", "code 1")).size(), 40U);
  EXPECT_TRUE(cache.find(Script("u", "code 2")).empty());
  EXPECT_TRUE(cache.find(Script("v", "code 1")).empty());
  EXPECT_TRUE(cache.find(Script("u", "code 10")).empty());
  cache.store(Script("u", "code 1"), span(fifty));  // replaced
  EXPECT_EQ(cache.size(), 1U);
  EXPECT_EQ(cache.bytes(), 50U);
  cache.store(Script("v", "code 2"), span(forty));
  cache.store(Script("w", "code 3"), span(thirty));  // 120 bytes: the least recently used one goes
  EXPECT_EQ(cache.size(), 2U);
  EXPECT_EQ(cache.bytes(), 70U);
  EXPECT_TRUE(cache.find(Script("u", "code 1")).empty());
  EXPECT_EQ(cache.find(Script("v", "code 2")).size(), 40U);  // now more recent than w
  cache.store(Script("x", "code 4"), span(forty));
  EXPECT_TRUE(cache.find(Script("w", "code 3")).empty());
  EXPECT_EQ(cache.bytes(), 80U);
  cache.store(Script("y", "code 5"), span(bytes(101)));  // larger than the cache
  EXPECT_TRUE(cache.find(Script("y", "code 5")).empty());
  EXPECT_EQ(cache.size(), 2U);
}

#if AERONET_WANT_MALLOC_OVERRIDES
// A bytecode that cannot be copied leaves its entry, and the size of the cache, as they were.
TEST(WebNetworkTest, ScriptCacheFailedCopy) {
  internal::ScriptCache cache(1000);
  const std::string small(40, 's');
  const std::string large(400, 'l');
  const internal::ScriptCache::Script script("u", "code");
  cache.store(script, std::as_bytes(std::span(small)));
  // The copy of a larger bytecode allocates a new block (see RawBytes::assign()).
  test::FailNextMalloc();
  EXPECT_THROW(cache.store(script, std::as_bytes(std::span(large))), std::bad_alloc);
  test::ResetToSysMalloc();
  EXPECT_EQ(cache.bytes(), 40U);
  EXPECT_EQ(cache.find(script).size(), 40U);
  cache.store(script, std::as_bytes(std::span(large)));
  EXPECT_EQ(cache.bytes(), 400U);
}
#endif

// The large scripts are compiled once per client: the next pages read their bytecode back, with the same behavior.
TEST(WebNetworkTest, ScriptsAreReadBackFromTheCache) {
  WebClient client(Config());
  const std::string padding = "//" + std::string(internal::ScriptCache::kMinScriptSize, 'p') + "\n";
  const std::string code = padding +
                           "function square(n) { return n * n; }"
                           "window.out = [square(7), square.toString(), new Error('e').stack.includes('/js?src=')];";
  const std::string html = "<script src='" + Url(test::ScriptPath(code)) +
                           "'></script>"
                           "<script>" +
                           padding +
                           "window.out.push(typeof square)</script>"
                           "<script src='" +
                           Url("/version.js") + "'></script>";
  const std::string expected = R"([49,"function square(n) { return n * n; }",true,"function"])";
  WebPage first = test::Navigate(client, test::PageUrl(gServer, html));
  EXPECT_EQ(test::Eval(first, "out"), expected);
  const std::string version = test::Eval(first, "version");
  WebPage second = test::Navigate(client, test::PageUrl(gServer, html));
  EXPECT_EQ(test::Eval(second, "out"), expected);
  // A script of the same URL with another content is compiled again.
  EXPECT_NE(test::Eval(second, "version"), version);
  EXPECT_TRUE(second.uncaughtErrors().empty());
  // A large script that does not compile is not cached, and reported each time.
  for (int load = 0; load < 2; ++load) {
    WebPage invalid = test::Navigate(client, test::PageUrl(gServer, "<script>" + padding + "window.x = (</script>"));
    ASSERT_EQ(invalid.uncaughtErrors().size(), 1U);
    EXPECT_TRUE(invalid.uncaughtErrors()[0].starts_with("SyntaxError")) << invalid.uncaughtErrors()[0];
  }

  WebClientConfig config = Config();
  config.maxScriptCacheBytes = 0;
  WebClient uncached(config);
  WebPage page = test::Navigate(uncached, test::PageUrl(gServer, html));
  EXPECT_EQ(test::Eval(page, "out"), expected);
}

// --- URL, URLSearchParams, TextEncoder, TextDecoder ---

TEST(WebNetworkTest, Url) {
  WebClient client(Config());
  EXPECT_EQ(
      Out(client, R"(const u = new URL('../b/c?x=1&y=a+b#h', 'https://user:pw@example.com:8443/a/d');
    out = [u.href, u.origin, u.protocol, u.username, u.password, u.host, u.hostname, u.port, u.pathname, u.search,
      u.hash, u.searchParams.get('y'), String(u), JSON.stringify({ u })];
    u.search = 'z=2';
    u.hash = '#top';
    u.pathname = 'p q';
    u.port = '443x';
    out.push(u.href);
    u.hostname = 'other.example';
    u.username = 'a b';
    u.password = '';
    u.protocol = 'http';
    u.searchParams.append('k', 'v w');
    out.push(u.href, u.searchParams.get('z'));
    u.search = '';
    u.hash = '';
    u.host = 'h.example:81';
    u.host = 'i.example:';
    out.push(u.host);
    u.host = 'j.example:abc';
    out.push(u.host);
    u.host = 'k.\texample:8\n3';
    out.push(u.host);
    u.port = '8\r4';
    out.push(u.port);
    u.port = '\t85';
    out.push(u.port);
    u.port = 'abc';
    out.push(u.port);
    u.port = '';
    out.push(u.port);
    u.href = 'http://e.example/?q=1';
    out.push(u.href, u.searchParams.get('q'), u.host);
    try { u.href = 'nope'; } catch (e) { out.push(e.name); }
    try { new URL('/relative'); } catch (e) { out.push(e.name); }
    const mail = new URL('mailto:a@b.example');
    out.push(URL.canParse('nope'), URL.canParse('/x', 'https://e.example'), URL.parse('nope'),
      URL.parse('/x', 'https://e.example').href, mail.origin, mail.pathname, mail.host,
      new URL('/x', location.href).href === location.origin + '/x', new URL('http://e.example').pathname,
      URL.canParse('/x', ''));)"),
      R"(["https://user:pw@example.com:8443/b/c?x=1&y=a+b#h","https://example.com:8443","https:","user","pw",)"
      R"("example.com:8443","example.com","8443","/b/c","?x=1&y=a+b","#h","a b",)"
      R"("https://user:pw@example.com:8443/b/c?x=1&y=a+b#h","{\"u\":\"https://user:pw@example.com:8443/b/c?x=1&y=a+b#h\"}",)"
      R"("https://user:pw@example.com/p%20q?z=2#top","http://a%20b@other.example/p%20q?z=2&k=v+w#top","2",)"
      R"("i.example:81","j.example:81","k.example:83","84","85","85","","http://e.example/?q=1","1","e.example",)"
      R"("TypeError","TypeError",false,true,null,"https://e.example/x","null","a@b.example","",true,"/",false])");
}

TEST(WebNetworkTest, UrlSearchParams) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(const p = new URLSearchParams('?a=1&b=x%20y&a=2&c=%E2%82%AC&d=%ZZ&e&&f=%2');
    out = [p.get('a'), p.getAll('a'), p.get('b'), p.get('c'), p.get('d'), p.get('e'), p.get('f'), p.get('g'),
      p.has('a', '2'), p.has('a', '3'), p.size, String(p)];
    p.delete('a', '1');
    p.set('b', '!');
    p.set('new', 'n');
    p.sort();
    const each = [];
    p.forEach((value, name) => each.push(name + '=' + value));
    out.push(String(p), each, [...p.keys()], [...p.values()].length, [...p.entries()][0],
      String(new URLSearchParams({ x: 1, s: "*-._~!'() \u{1F600}" })),
      new URLSearchParams([['k', 'v'], ['k', 'w']]).getAll('k'), String(new URLSearchParams(p)));
    p.delete('b');
    out.push(String(p), String(new URLSearchParams('a=%F0%9F%98%80+b')));
    try { new URLSearchParams([['a']]); } catch (e) { out.push(e.name); })"),
            R"(["1",["1","2"],"x y","€","%ZZ","","%2",null,true,false,7,"a=1&b=x+y&a=2&c=%E2%82%AC&d=%25ZZ&e=&f=%252",)"
            R"("a=2&b=%21&c=%E2%82%AC&d=%25ZZ&e=&f=%252&new=n",["a=2","b=!","c=€","d=%ZZ","e=","f=%2","new=n"],)"
            R"(["a","b","c","d","e","f","new"],7,["a","2"],"x=1&s=*-._%7E%21%27%28%29+%F0%9F%98%80",["v","w"],)"
            R"("a=2&b=%21&c=%E2%82%AC&d=%25ZZ&e=&f=%252&new=n","a=2&c=%E2%82%AC&d=%25ZZ&e=&f=%252&new=n",)"
            R"("a=%F0%9F%98%80+b","TypeError"])");
}

TEST(WebNetworkTest, TextEncoderAndDecoder) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(const encoder = new TextEncoder();
    const bytes = encoder.encode('a€\u{1F600}');
    const into = new Uint8Array(5);
    const result = encoder.encodeInto('a€\u{1F600}', into);
    const whole = new Uint8Array(10);
    const decoder = new TextDecoder();
    out = [encoder.encoding, [...bytes], result, [...into], encoder.encodeInto('ab', whole), [...encoder.encode('\uD800')],
      [...encoder.encode()], decoder.decode(bytes), decoder.decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x61])),
      new TextDecoder('UTF8', { ignoreBOM: true }).decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x61])).length,
      decoder.decode(), decoder.decode(new Uint8Array([0xFF])), decoder.decode(bytes.buffer),
      decoder.decode(new DataView(bytes.buffer, 1, 3)), decoder.decode(new Uint8Array()), decoder.encoding,
      decoder.fatal, decoder.ignoreBOM, new TextDecoder('utf-8', { fatal: true }).fatal];
    // UTF-8 only: the bytes of a UTF-16 byte order mark are invalid. Invalid sequences throw with fatal.
    const utf16 = new Uint8Array([0xFF, 0xFE, 0x61, 0x00]);
    const fatal = new TextDecoder('utf-8', { fatal: true });
    out.push([...decoder.decode(utf16)].map(c => c.codePointAt(0)), fatal.decode(bytes),
      [...new TextDecoder('utf-8', { ignoreBOM: true, fatal: true }).decode(new Uint8Array([0xEF, 0xBB, 0xBF]))]
        .map(c => c.codePointAt(0)));
    for (const bad of [() => new TextDecoder('latin1'), () => decoder.decode('str'), () => fatal.decode(utf16)]) {
      try { bad(); } catch (e) { out.push(e.name); }
    })"),
            R"(["utf-8",[97,226,130,172,240,159,152,128],{"read":2,"written":4},[97,226,130,172,0],)"
            R"({"read":2,"written":2},[239,191,189],[],"a€😀","a",2,"",")"
            "\xEF\xBF\xBD"
            R"(","a€😀","€","","utf-8",false,false,)"
            R"(true,[65533,65533,97,0],"a€😀",[65279],"RangeError","TypeError","TypeError"])");
}

// The bodies of fetch() are UTF-8 (Encoding "UTF-8 decode"): a UTF-8 byte order mark is dropped, whatever their
// Content-Type, and those of UTF-16 are invalid bytes. XMLHttpRequest decodes its text with the charset of the
// response, but its JSON as UTF-8.
TEST(WebNetworkTest, BodiesAreUtf8) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(const codes = text => [...text].map(c => c.codePointAt(0));
    const utf16 = new Uint8Array([0xFF, 0xFE, 0x61, 0x00]);
    const latin1 = '/api?type=' + encodeURIComponent('text/plain; charset=iso-8859-1') + '&body=%E9';
    Promise.all([new Response(utf16).text().then(codes),
      new Response(new Uint8Array([0xEF, 0xBB, 0xBF, 0x7B, 0x7D])).json(),
      fetch(latin1).then(r => r.text()).then(codes),
      new Promise(resolve => {
        const xhr = new XMLHttpRequest();
        xhr.onload = () => resolve(codes(xhr.responseText));
        xhr.open('GET', latin1);
        xhr.send();
      }),
      new Promise(resolve => {
        const xhr = new XMLHttpRequest();
        xhr.responseType = 'json';
        xhr.onload = () => resolve(xhr.response);
        xhr.open('GET', '/api?type=' + encodeURIComponent('application/json; charset=iso-8859-1') + '&body=' +
          encodeURIComponent('"\u00e9"'));
        xhr.send();
      }),
      String(new URLSearchParams('a=%FF%FEa').get('a').length),
    ]).then(results => { out = results; }))"),
            R"([[65533,65533,97,0],{},[65533],[233],")"
            "\xC3\xA9"
            R"(","3"])");
}

// --- Page scripts that change what the prelude uses ---

// The prelude reads the arguments of its natives from objects that page scripts can change (the getters of
// Request.prototype, Array.prototype.join(), the global Uint8Array, String): the natives check them, and a conversion
// that runs a script (toString()) cannot change what they read before it.

// The functions of these tests, in the page: 'throwing' cannot become a string, settle(promise) is the status of a
// response or the error, fetchWith(name, value, url, init) is fetch(url, init) with the getter 'name' of
// Request.prototype returning 'value'.
constexpr std::string_view kTamperHelpers = R"(
  const throwing = { toString() { throw new Error('tampered'); } };
  const settle = promise => promise.then(r => r.status, e => e.name + ': ' + e.message);
  const fetchWith = (name, value, url, init) => {
    const descriptor = Object.getOwnPropertyDescriptor(Request.prototype, name);
    Object.defineProperty(Request.prototype, name, { get: () => value, configurable: true });
    try {
      return fetch(url, init);
    } finally {
      Object.defineProperty(Request.prototype, name, descriptor);
    }
  };
)";

TEST(WebNetworkTest, ChangedGettersOfTheRequests) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, std::string(kTamperHelpers) + R"(
    Object.prototype.outOfRange = 3;
    const results = [
      settle(fetchWith('method', throwing, '/api')),
      settle(fetchWith('method', 'CONNECT', '/api')),
      settle(fetchWith('method', 'TRACE', '/api')),
      settle(fetchWith('method', 'GET', '/api', { method: 'POST', body: 'x' })),
      settle(fetchWith('method', 'HEAD', '/api', { method: 'POST', body: 'x' })),
      settle(fetchWith('url', throwing, '/api')),
      settle(fetchWith('url', 'http://user@127.0.0.1:1/', '/api')),
      settle(fetchWith('mode', 'toString', '/api')),
      settle(fetchWith('mode', 'outOfRange', '/api')),
      settle(fetchWith('credentials', 'toString', '/api')),
    ];
    delete Object.prototype.outOfRange;
    Promise.all(results).then(r => { out = r; });)"),
            R"(["Error: tampered","TypeError: aeronet: the HTTP method 'CONNECT' is not supported",)"
            R"("TypeError: aeronet: the HTTP method 'TRACE' is not supported",)"
            R"("TypeError: aeronet: a GET request has no body","TypeError: aeronet: a HEAD request has no body",)"
            R"("Error: tampered",)"
            R"("TypeError: aeronet: only http and https URLs can be fetched, not 'http://user@127.0.0.1:1/'",)"
            R"("TypeError: aeronet: invalid mode or credentials of a request",)"
            R"("TypeError: aeronet: invalid mode or credentials of a request",)"
            R"("TypeError: aeronet: invalid mode or credentials of a request"])");
}

// The header lines of a request are converted before its URL is resolved: a conversion that resolves another URL
// (here to another origin, which does not allow the document to read its responses) does not change where the
// request goes, nor what the document can read.
TEST(WebNetworkTest, ChangedHeaderLines) {
  WebClient client(Config());
  const std::string ownBody(24, 'o');
  EXPECT_EQ(Out(client, std::string(kTamperHelpers) + "const own = '" + Url("/api?body=" + ownBody) +
                            "'; const other = '" + SameSiteUrl("/api?body=") + R"(';
    // fetch() with the header lines replaced by lines(joined), joined being the real ones.
    const fetchWithLines = (lines, url, init) => {
      const join = Array.prototype.join;
      Array.prototype.join = function (separator) {
        const joined = join.call(this, separator);
        if (separator !== '\n') return joined;
        Array.prototype.join = join;
        return lines(joined);
      };
      try {
        return fetch(url, init);
      } finally {
        Array.prototype.join = join;
      }
    };
    // A URL of the other origin of the same length: the URL resolver overwrites its last result in place.
    const sameLength = other.padEnd(own.length, 'x');
    Promise.all([
      settle(fetchWithLines(() => throwing, '/api')),
      settle(fetchWithLines(() => 'bad name:x', '/api')),
      fetchWithLines(() => 'x-no-colon', '/api').then(r => r.json()).then(e => e.headers['x-no-colon'] ?? null),
      fetchWithLines(joined => ({ toString: () => { new URL(sameLength); return joined; } }), own)
        .then(r => r.text(), e => e.message),
    ]).then(r => { out = r; });)"),
            R"(["Error: tampered","TypeError: aeronet: invalid header 'bad name: x'","",")" + ownBody + R"("])");
}

// The prelude makes the Uint8Array of the bodies with the global one.
TEST(WebNetworkTest, ChangedUint8Array) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, std::string(kTamperHelpers) + R"(
    const withFakeUint8Array = run => {
      const real = Uint8Array;
      Uint8Array = function () { return { length: 1, byteLength: 1 }; };
      try {
        return run();
      } catch (e) {
        return e.name;
      } finally {
        Uint8Array = real;
      }
    };
    const body = new Uint8Array([65]);
    const xhr = new XMLHttpRequest();
    xhr.open('GET', '/api?body=abc', false);
    xhr.send();
    Promise.all([
      withFakeUint8Array(() => fetch('/api', { method: 'POST', body })).then(r => r.status, e => e.name),
      fetch('/api?body=abc').then(r => withFakeUint8Array(() => r.text())).catch(e => e.name),
      withFakeUint8Array(() => xhr.responseText),
      xhr.responseText,
    ]).then(r => { out = r; });)"),
            R"(["TypeError","TypeError","TypeError","abc"])");
}

// Response.json() parses what text() returns as JSON.parse() would, also a value other than a string from a page that
// replaces text().
TEST(WebNetworkTest, ChangedTextOfAResponse) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, R"(const json = value => {
      Response.prototype.text = async () => value;
      return new Response('').json().catch(e => e.name + ': ' + e.message);
    };
    (async () => {
      out = [await json(42), await json({ toString: () => '{"a":[1]}' }),
        await json({ toString() { throw new RangeError('no text'); } }), (await json('{"a":')).split(':')[0]];
    })();)"),
            R"([42,{"a":[1]},"RangeError: no text","SyntaxError"])");
}

// The strings that the prelude passes through String() or a header getter of XMLHttpRequest.
TEST(WebNetworkTest, ChangedStringConversions) {
  WebClient client(Config());
  EXPECT_EQ(Out(client, std::string(kTamperHelpers) + R"(
    const messageOf = run => {
      try {
        return run();
      } catch (e) {
        return e.message;
      }
    };
    // run() with String() returning 'throwing' at its call number 'call'.
    const withString = (call, run) => {
      const real = String;
      let calls = 0;
      String = value => ++calls === call ? throwing : real(value);
      try {
        return messageOf(run);
      } finally {
        String = real;
      }
    };
    const encoder = new TextEncoder();
    const wellFormedThrowing = { isWellFormed: () => true, toString: throwing.toString };
    const xhr = new XMLHttpRequest();
    xhr.open('GET', '/api?body=abc', false);
    xhr.send();
    xhr.getResponseHeader = () => throwing;
    out = [
      withString(1, () => new URL('http://a.example/')),
      withString(2, () => new URL('/a', 'http://a.example/')),
      messageOf(() => {
        const real = String;
        String = () => wellFormedThrowing;
        try {
          return encoder.encode('x');
        } finally {
          String = real;
        }
      }),
      messageOf(() => xhr.responseText),
    ];)"),
            R"(["tampered","tampered","tampered","tampered"])");
}

// --- The rules of the Fetch Standard ---

TEST(WebNetworkTest, ForbiddenRequestHeaders) {
  using internal::IsForbiddenRequestHeader;
  for (const std::string_view name : {"accept-charset",
                                      "accept-encoding",
                                      "access-control-request-headers",
                                      "access-control-request-method",
                                      "connection",
                                      "content-length",
                                      "cookie",
                                      "cookie2",
                                      "date",
                                      "dnt",
                                      "expect",
                                      "host",
                                      "keep-alive",
                                      "origin",
                                      "referer",
                                      "set-cookie",
                                      "te",
                                      "trailer",
                                      "transfer-encoding",
                                      "upgrade",
                                      "via",
                                      "proxy-a",
                                      "sec-ch-ua"}) {
    EXPECT_TRUE(IsForbiddenRequestHeader(name, "x")) << name;
  }
  EXPECT_FALSE(IsForbiddenRequestHeader("accept", "x"));
  EXPECT_FALSE(IsForbiddenRequestHeader("x-proxy-a", "x"));
  EXPECT_TRUE(IsForbiddenRequestHeader("x-http-method", "GET, track"));
  EXPECT_TRUE(IsForbiddenRequestHeader("x-method-override", "Connect"));
  EXPECT_FALSE(IsForbiddenRequestHeader("x-http-method-override", "PUT"));
}

TEST(WebNetworkTest, CorsSafelistedRequestHeaders) {
  using internal::IsCorsSafelistedRequestHeader;
  EXPECT_TRUE(IsCorsSafelistedRequestHeader("accept", "text/html, */*;q=0.8"));
  EXPECT_FALSE(IsCorsSafelistedRequestHeader("accept", "a(b"));
  EXPECT_FALSE(IsCorsSafelistedRequestHeader("accept", "a\x01"));
  EXPECT_TRUE(IsCorsSafelistedRequestHeader("accept", "a\tb"));
  EXPECT_FALSE(IsCorsSafelistedRequestHeader("accept", std::string(129, 'a')));
  EXPECT_TRUE(IsCorsSafelistedRequestHeader("accept-language", "en-US,fr;q=0.9 *"));
  EXPECT_FALSE(IsCorsSafelistedRequestHeader("content-language", "fr_FR"));
  EXPECT_FALSE(IsCorsSafelistedRequestHeader("accept-language", "fr~"));
  EXPECT_TRUE(IsCorsSafelistedRequestHeader("content-type", "Text/Plain; charset=utf-8"));
  EXPECT_TRUE(IsCorsSafelistedRequestHeader("content-type", "multipart/form-data; boundary=x"));
  EXPECT_TRUE(IsCorsSafelistedRequestHeader("content-type", "application/x-www-form-urlencoded"));
  EXPECT_FALSE(IsCorsSafelistedRequestHeader("content-type", "application/json"));
  EXPECT_FALSE(IsCorsSafelistedRequestHeader("content-type", "text/plain; a=\"b\""));
  for (const std::string_view range : {"bytes=0-", "bytes=5-10", "bytes=007-7", "bytes=0-0"}) {
    EXPECT_TRUE(IsCorsSafelistedRequestHeader("range", range)) << range;
  }
  for (const std::string_view range : {"bytes=10-5", "bytes=21-12", "bytes=-5", "bytes=a-b", "bytes=5", "items=0-",
                                       "bytes=1-2,3-4", "bytes=99-100x"}) {
    EXPECT_FALSE(IsCorsSafelistedRequestHeader("range", range)) << range;
  }
  EXPECT_FALSE(IsCorsSafelistedRequestHeader("x-custom", "x"));
}

TEST(WebNetworkTest, CorsChecks) {
  using internal::CorsCheck;
  constexpr std::string_view kOrigin = "http://a.example";
  HttpResponse none;
  EXPECT_FALSE(CorsCheck(none, kOrigin, false));
  HttpResponse star;
  star.headerAddLine("access-control-allow-origin", "*");
  EXPECT_TRUE(CorsCheck(star, kOrigin, false));
  EXPECT_FALSE(CorsCheck(star, kOrigin, true));
  HttpResponse own;
  own.headerAddLine("access-control-allow-origin", kOrigin);
  EXPECT_TRUE(CorsCheck(own, kOrigin, false));
  EXPECT_FALSE(CorsCheck(own, kOrigin, true));
  EXPECT_FALSE(CorsCheck(own, "http://b.example", false));
  own.headerAddLine("access-control-allow-credentials", "true");
  EXPECT_TRUE(CorsCheck(own, kOrigin, true));

  using internal::PreflightAllows;
  HttpResponse failed(http::StatusCodeInternalServerError);
  failed.headerAddLine("access-control-allow-origin", "*");
  EXPECT_FALSE(PreflightAllows(failed, kOrigin, false, http::Method::GET, ""));
  HttpResponse allowed(http::StatusCodeNoContent);
  allowed.headerAddLine("access-control-allow-origin", "*")
      .headerAddLine("access-control-allow-methods", "PUT, PATCH")
      .headerAddLine("access-control-allow-headers", "X-A,x-b");
  EXPECT_TRUE(PreflightAllows(allowed, kOrigin, false, http::Method::PUT, "x-a,x-b"));
  EXPECT_TRUE(PreflightAllows(allowed, kOrigin, false, http::Method::POST, ""));
  EXPECT_FALSE(PreflightAllows(allowed, kOrigin, false, http::Method::DELETE, ""));
  EXPECT_FALSE(PreflightAllows(allowed, kOrigin, false, http::Method::PUT, "x-a,x-c"));
  EXPECT_TRUE(PreflightAllows(allowed, kOrigin, false, http::Method::HEAD, ""));
  // An informational status is not a success, and the response must pass the CORS check.
  HttpResponse informational(http::StatusCodeSwitchingProtocols);
  informational.headerAddLine("access-control-allow-origin", "*");
  EXPECT_FALSE(PreflightAllows(informational, kOrigin, false, http::Method::GET, ""));
  EXPECT_FALSE(PreflightAllows(HttpResponse(http::StatusCodeNoContent), kOrigin, false, http::Method::GET, ""));
  // With credentials, "*" allows no header.
  HttpResponse withCredentials(http::StatusCodeNoContent);
  withCredentials.headerAddLine("access-control-allow-origin", kOrigin)
      .headerAddLine("access-control-allow-credentials", "true")
      .headerAddLine("access-control-allow-headers", "*");
  EXPECT_TRUE(PreflightAllows(withCredentials, kOrigin, true, http::Method::GET, ""));
  EXPECT_FALSE(PreflightAllows(withCredentials, kOrigin, true, http::Method::GET, "x-a"));
  EXPECT_TRUE(PreflightAllows(withCredentials, kOrigin, false, http::Method::GET, "x-a"));
}

TEST(WebNetworkTest, ExposedResponseHeaderNames) {
  using internal::IsExposedResponseHeader;
  EXPECT_TRUE(IsExposedResponseHeader("content-type", "", true));
  EXPECT_TRUE(IsExposedResponseHeader("pragma", "", false));
  EXPECT_FALSE(IsExposedResponseHeader("x-a", "", false));
  EXPECT_TRUE(IsExposedResponseHeader("x-a", "x-b, X-A", true));
  EXPECT_TRUE(IsExposedResponseHeader("x-a", "*", false));
  EXPECT_FALSE(IsExposedResponseHeader("x-a", "*", true));
  EXPECT_FALSE(IsExposedResponseHeader("set-cookie", "*, set-cookie", false));
  EXPECT_FALSE(IsExposedResponseHeader("set-cookie2", "set-cookie2", false));
}

TEST(WebNetworkTest, Referrers) {
  using internal::ReferrerOf;
  constexpr std::string_view kDocument = "https://a.example:8443/p/q?x#f";
  constexpr std::string_view kOrigin = "https://a.example:8443";
  EXPECT_EQ(ReferrerOf(kDocument, kOrigin, "https://a.example:8443/other"), "https://a.example:8443/p/q?x");
  EXPECT_EQ(ReferrerOf(kDocument, kOrigin, "https://b.example/"), "https://a.example:8443/");
  EXPECT_EQ(ReferrerOf(kDocument, kOrigin, "http://a.example:8443/"), "");
  EXPECT_EQ(ReferrerOf("http://a.example/", "http://a.example", "https://b.example/"), "http://a.example/");
}
}  // namespace
}  // namespace aeronet
