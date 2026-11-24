// The ES modules of the pages against local servers: module scripts and their order, static imports, import(),
// import.meta, the failures of a graph (fetch, parse, link, evaluation), the cors mode of the module requests, the
// redirects, the inserted module scripts, the limits and the bytecode cache.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "../src/js-module.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/web-session.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/web-client-config.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"
#include "web-test-util.hpp"

namespace aeronet {
namespace {

// The requests to /m/, counted from the thread of the server.
std::atomic<int> gModuleRequests{0};

// A module served at a fixed path.
struct StaticModule {
  std::string_view path;
  std::string_view code;
};

// The modules whose imports form cycles, at fixed paths: their URLs cannot hold their code.
constexpr StaticModule kStaticModules[]{
    {"/static/a.js", "import { b, getA } from './b.js'; export const a = 'a'; log.push(b, getA());"},
    {"/static/b.js", "import { a } from './a.js'; export const b = 'b'; export function getA() { return a; }"},
    // A cycle whose first module also imports a missing one.
    {"/static/c.js", "import './d.js'; import './missing.js'; log.push('c');"},
    {"/static/d.js", "import './c.js'; log.push('d');"},
    // A cycle reached by a module that imports a missing one after it: y.js loads healthy, its importer does not.
    {"/static/e.js", "import './f.js'; import './missing.js'; log.push('e');"},
    {"/static/f.js", "import './y.js'; import './e.js'; log.push('f');"},
    {"/static/y.js", "log.push('y');"},
    // A module importing f.js after the failure of the cycle: it fails without running y.js.
    {"/static/g.js", "import './f.js'; log.push('g');"},
    // A module (j.js) that completes inside a cycle (h.js and i.js) by importing a module of the cycle that completed
    // before it (i.js), before the first module of the cycle (h.js) fails: it reaches the failure too.
    {"/static/h.js", "import './i.js'; import './j.js'; import './missing.js'; log.push('h');"},
    {"/static/i.js", "import './h.js'; log.push('i');"},
    {"/static/j.js", "import './k.js'; import './i.js'; log.push('j');"},
    {"/static/k.js", "log.push('k');"},
};

// The size of the string literal of each /large/ module: their bytecode is a little larger.
constexpr std::size_t kLargeLiteralSize = 4000;

// 'str' as a JSON string, for the plain ASCII of the headers of the tests.
std::string JsonString(std::string_view str) {
  std::string json = "\"";
  for (const char ch : str) {
    if (ch == '"' || ch == '\\') {
      json.push_back('\\');
    }
    json.push_back(ch);
  }
  return json + '"';
}

// /m/*: the module of the query parameter 'src', with the Content-Type 'type' (text/javascript without it), the status
// 'status', and the Access-Control-Allow-Origin 'acao' ('origin' echoes the Origin of the request) and
// Access-Control-Allow-Credentials 'acac'. With 'echo', it also exports the Origin, the Cookie and the Referer of its
// request. With 'to', it redirects there.
HttpResponse ModuleResponse(const HttpRequestView& req) {
  ++gModuleRequests;
  if (const auto to = req.queryParamValue("to")) {
    HttpResponse resp = req.makeResponse(http::StatusCodeFound);
    resp.location(*to);
    return resp;
  }
  std::string code(req.queryParamValueOrEmpty("src"));
  if (req.queryParamValue("echo")) {
    code += "export const origin = " + JsonString(req.headerValueOrEmpty("origin")) +
            "; export const cookie = " + JsonString(req.headerValueOrEmpty(http::Cookie)) +
            "; export const referer = " + JsonString(req.headerValueOrEmpty("referer")) + ';';
  }
  const std::string_view status = req.queryParamValueOrEmpty("status");
  const std::string_view type = req.queryParamValueOrEmpty("type");
  HttpResponse resp = req.makeResponse(
      status.empty() ? http::StatusCodeOK : static_cast<http::StatusCode>(std::stoi(std::string(status))), code,
      type.empty() ? std::string_view("text/javascript") : type);
  if (const std::string_view acao = req.queryParamValueOrEmpty("acao"); !acao.empty()) {
    resp.headerAddLine("access-control-allow-origin", acao == "origin" ? req.headerValueOrEmpty("origin") : acao);
  }
  if (req.queryParamValue("acac")) {
    resp.headerAddLine("access-control-allow-credentials", "true");
  }
  return resp;
}

// /chain?n=<n>: a module importing that of n - 1, down to 0, exporting its depth. With 'delay', each one is answered
// after that many milliseconds.
HttpResponse ChainResponse(const HttpRequestView& req) {
  const int depth = std::stoi(std::string(req.queryParamValueOrEmpty("n")));
  const std::string_view delay = req.queryParamValueOrEmpty("delay");
  if (!delay.empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds{std::stoi(std::string(delay))});
  }
  std::string code = "export const depth = 0;";
  if (depth != 0) {
    code = "import { depth as below } from '/chain?n=" + std::to_string(depth - 1) +
           (delay.empty() ? std::string() : "&delay=" + std::string(delay)) + "'; export const depth = below + 1;";
  }
  return req.makeResponse(code, "text/javascript");
}

// /filler/<name>: modules of code 'code' followed by an array literal of 'elements' elements, whose compilation takes
// tens of kilobytes of memory: more than what the setup of a page leaves free after its garbage collection.
HttpResponse FillerResponse(const HttpRequestView& req) {
  static constexpr StaticModule kFillers[]{
      {"/filler/a.js", "import { b } from './b.js'; export const a = b + 1;"},
      {"/filler/b.js", "export const b = 2;"},
      {"/filler/d.js", "export default 4;"},
  };
  static constexpr int kElements = 3000;
  for (const StaticModule& filler : kFillers) {
    if (req.path() == filler.path) {
      std::string body(filler.code);
      body += "const filler = [";
      for (int element = 0; element < kElements; ++element) {
        body += "1,";
      }
      return req.makeResponse(body + "];", "text/javascript");
    }
  }
  return req.makeResponse(http::StatusCodeNotFound);
}

test::TestServer MakeServer() {
  test::TestServer server = test::MakePageServer();
  auto router = server.router();
  router.setPath(http::Method::GET, "/m/*", ModuleResponse);
  router.setPath(http::Method::GET, "/chain", ChainResponse);
  router.setPath(http::Method::GET, "/filler/*", FillerResponse);
  // /tiny?k=<k>: a module exporting k.
  router.setPath(http::Method::GET, "/tiny", [](const HttpRequestView& req) {
    return req.makeResponse("export const v = " + std::string(req.queryParamValueOrEmpty("k")) + ";",
                            "text/javascript");
  });
  router.setPath(http::Method::GET, "/static/*", [](const HttpRequestView& req) {
    for (const StaticModule& module : kStaticModules) {
      if (req.path() == module.path) {
        return req.makeResponse(module.code, "text/javascript");
      }
    }
    return req.makeResponse(http::StatusCodeNotFound);
  });
  // /large/root.js imports /large/dep.js, each holding a string literal of kLargeLiteralSize characters.
  router.setPath(http::Method::GET, "/large/*", [](const HttpRequestView& req) {
    const std::string literal = '\'' + std::string(kLargeLiteralSize, 'x') + '\'';
    if (req.path() == "/large/root.js") {
      return req.makeResponse("import { dep } from './dep.js'; log.push(dep.length, " + literal + ".length);",
                              "text/javascript");
    }
    return req.makeResponse("export const dep = " + literal + ';', "text/javascript");
  });
  return server;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gServer = MakeServer();
// Another origin of the same site (127.0.0.1, another port): its cookies are those of gServer.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gOther = MakeServer();

// The configuration of the clients of these tests: a JavaScript call takes much more native stack with the
// sanitizers (see WebRuntimeTest.StackLimit).
WebClientConfig Config() {
  WebClientConfig config;
  config.maxScriptStackSize = std::size_t{4} << 20U;
  return config;
}

std::string Page(std::string_view html) { return test::PageUrl(gServer, html); }

// The query of the module code 'code'.
std::string Src(std::string_view code) { return "?src=" + test::PercentEncode(code); }

// The path of the module 'code' at /m/<path>, followed by the query parameters 'params' ("&type=...").
std::string Module(std::string_view path, std::string_view code, std::string_view params = {}) {
  return "/m/" + std::string(path) + Src(code) + std::string(params);
}

// The JSON of the global 'log' of the page of 'html', loaded until 'until'.
std::string LogOf(WebClient& client, std::string_view html, LoadState until = LoadState::Load) {
  WebPage page = test::Navigate(client, Page(html), {.until = until});
  EXPECT_TRUE(page.uncaughtErrors().empty()) << page.uncaughtErrors()[0];
  return test::Log(page);
}

// Skipped with a QuickJS-ng that does not free modules safely (see ModulesEnabled()).
class WebModuleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!internal::ModulesEnabled()) {
      GTEST_SKIP() << "QuickJS-ng without the fixes of its modules";
    }
  }
};

TEST_F(WebModuleTest, ModuleScriptsAreDeferred) {
  WebClient client(Config());
  const std::string log =
      LogOf(client,
            "<script>log = []; document.addEventListener('DOMContentLoaded', () => log.push('DOMContentLoaded'));"
            "addEventListener('load', () => log.push('load'))</script>"
            "<script type=module async>log.push('async')</script>"
            "<script type=module>log.push(['inline', document.currentScript, this === undefined]);"
            "document.write('<p id=written>')</script>"
            "<script type=module src='" +
                Module("a.js", "log.push('external')") +
                "'></script>"
                "<script defer src='" +
                test::ScriptPath("log.push('defer')") +
                "'></script>"
                "<script>log.push('classic')</script>"
                // The nomodule scripts are the fallback of the browsers without modules.
                "<script nomodule>log.push('nomodule')</script>"
                "<script type=module>log.push(document.getElementById('written'))</script>");
  EXPECT_EQ(log, R"(["classic",["inline",null,true],"external","defer",null,"DOMContentLoaded","async","load"])");
}

TEST_F(WebModuleTest, ImportsResolveAgainstTheUrlOfTheirModule) {
  WebClient client(Config());
  const std::string shared = "export let count = 0; export function inc() { return ++count; }";
  const std::string lib = "import { inc } from '../shared.js" + Src(shared) + "'; export const a = inc();";
  const std::string main = "import { a } from './lib/a.js" + Src(lib) +
                           "'; import { inc, count } from '/m/app/shared.js" + Src(shared) + "'; import { b } from '" +
                           test::ServerUrl(gServer) + Module("b.js", "export const b = 'b'") +
                           "'; log.push(a, inc(), count, b);";
  // The shared module runs once, its binding live.
  EXPECT_EQ(
      LogOf(client, "<script>log = []</script><script type=module src='" + Module("app/main.js", main) + "'></script>"),
      R"([1,2,2,"b"])");
}

TEST_F(WebModuleTest, ImportMeta) {
  WebClient client(Config());
  const std::string origin = test::ServerUrl(gServer);
  WebPage page =
      test::Navigate(client, Page("<base href='/m/base/'><script>log = []</script>"
                                  "<script type=module src='" +
                                  Module("meta/x.js",
                                         "log.push(import.meta.url.includes('/m/meta/x.js?src='), "
                                         "import.meta.resolve('./y.js'), Object.keys(import.meta))") +
                                  "'></script>"
                                  // An inline module has the base URL of its document.
                                  "<script type=module>log.push(import.meta.url, import.meta.resolve('../z.js'));"
                                  "try { import.meta.resolve('react') } catch (e) { log.push(e.name) }</script>"));
  EXPECT_EQ(test::Log(page), "[true,\"" + origin + "/m/meta/y.js\",[\"url\",\"resolve\"],\"" + origin +
                                 "/m/base/\",\"" + origin + "/m/z.js\",\"TypeError\"]");

  // After an inline module whose graph failed: QuickJS-ng gives the inline modules the import.meta of the first one.
  page = test::Navigate(client, Page("<script>log = []</script><script type=module>import '/static/c.js';</script>"
                                     "<script type=module>log.push(import.meta.url, import.meta.resolve('./z.js'))"
                                     "</script>"));
  const std::string pageUrl(page.url());
  EXPECT_EQ(test::Log(page), "[\"" + pageUrl + "\",\"" + origin + "/z.js\"]");
}

TEST_F(WebModuleTest, DynamicImports) {
  WebClient client(Config());
  const std::string value = "./value.js" + Src("export default 42");
  // A module starting with a byte order mark: dropped by its UTF-8 decoding.
  const std::string bom = Module("dyn/bom.js",
                                 "\xEF\xBB\xBF"
                                 "export default 'bom'");
  const std::string fromModule =
      "const { b } = await import('./b.js" + Src("export const b = 'b'") + "'); log.push(b);";
  WebPage page = test::Navigate(
      client, Page("<base href='/m/dyn/'><script>log = [];"
                   // From a classic script, against the base URL of the document.
                   "import('" +
                   value +
                   "').then(m => log.push(m.default));"
                   "Promise.all([import('" +
                   value + "'), import('" + value +
                   "')]).then(([x, y]) => log.push(x === y))</script>"
                   "<script type=module src='" +
                   Module("dyn/top.js", fromModule) + "'></script><script type=module>log.push((await import('" + bom +
                   "')).default)</script>"));
  EXPECT_EQ(test::Log(page), R"([42,true,"b","bom"])");
  // From evaluate(), against the base URL of the document too.
  EXPECT_EQ(test::Eval(page, "import('" + value + "').then(m => { log.push(m.default + 1) }); 0"), "0");
  EXPECT_EQ(test::Log(page), R"([42,true,"b","bom",43])");
  EXPECT_TRUE(page.uncaughtErrors().empty());
}

TEST_F(WebModuleTest, FailedDynamicImports) {
  WebClient client(Config());
  WebPage page = test::Navigate(
      client,
      Page("<script>log = []; (async () => { for (const [specifier, options] of ["
           "['react'], ['/m/missing.js?status=404'], ['data:text/javascript,export default 1'], "
           "['http://user@127.0.0.1:1/x.js'],"
           "['" +
           Module("text.js", "export default 1", "&type=text/plain") + "'], ['" +
           Module("syntax.js", "export default (") + "'], ['" + Module("json.js", "{}", "&type=application/json") +
           "', { with: { type: 'json' } }], ['" + test::ServerUrl(gOther) + Module("other.js", "export default 1") +
           // No attribute: a JavaScript module. Any key but type is a SyntaxError, before the type.
           "'], ['" + Module("attributes.js", "export default 1") + "', { with: {} }], ['" +
           Module("attributes.js", "export default 1") + "', { with: { foo: 'x' } }], ['" +
           Module("attributes.js", "export default 1") +
           "', { with: { type: 'json', foo: 'x' } }]]) { log.push(await import(specifier, options).then(() => "
           "'loaded', e => e.name)) } })()</script>"
           // The check of Vite for a browser with modules.
           "<script type=module>import.meta.url; import('_').catch(() => 1); (async function* () {})().next();"
           "window.modern = true</script>"));
  EXPECT_EQ(
      test::Log(page),
      R"(["TypeError","TypeError","TypeError","TypeError","TypeError","SyntaxError","TypeError","TypeError","loaded",)"
      R"("SyntaxError","SyntaxError"])");
  EXPECT_EQ(test::Eval(page, "modern"), "true");
  EXPECT_TRUE(page.uncaughtErrors().empty());

  // A module that failed fails again without request.
  const int requests = gModuleRequests;
  EXPECT_EQ(test::Eval(page, "import('/m/missing.js?status=404').catch(e => log.push(e.message)); 0"), "0");
  EXPECT_EQ(gModuleRequests, requests);
  EXPECT_TRUE(test::Log(page).contains("Failed to fetch module")) << test::Log(page);
}

TEST_F(WebModuleTest, FetchFailureOfAGraph) {
  WebClient client(Config());
  const std::string root =
      "import './dep.js" + Src("log.push('dep')") + "'; import './missing.js?status=404'; log.push('root')";
  WebPage page =
      test::Navigate(client, Page("<script>log = []</script>"
                                  "<script type=module src='" +
                                  Module("graph.js", root) +
                                  "' onerror=\"log.push('error')\" onload=\"log.push('load')\"></script>"
                                  "<script type=module onerror=\"log.push('inline error')\">" +
                                  root +
                                  "</script>"
                                  "<script type=module src='' onerror=\"log.push('empty src')\"></script>"
                                  "<script type=module src='" +
                                  Module("ok.js", "log.push('ok')") + "' onload=\"log.push('ok load')\"></script>"));
  // Nothing of a graph that failed runs, nor is reported: an error event.
  EXPECT_EQ(test::Log(page), R"(["error","inline error","empty src","ok","ok load"])");
  EXPECT_TRUE(page.uncaughtErrors().empty()) << page.uncaughtErrors()[0];
}

TEST_F(WebModuleTest, ErrorsOfAGraph) {
  WebClient client(Config());
  WebPage page = test::Navigate(
      client, Page("<script>log = []</script>"
                   // A parse error of an import: reported, nothing of the graph runs, the script still loads.
                   "<script type=module src='" +
                   Module("parse.js", "import './dep.js" + Src("log.push('dep')") + "'; import './bad.js" +
                                          Src("export default (") + "'; log.push('root')") +
                   "' onload=\"log.push('load')\"></script>"
                   // A link error: an import that its module does not export.
                   "<script type=module>import { missing } from '" +
                   Module("x.js", "export const x = 1") +
                   "'</script>"
                   // An evaluation error, at once or after an await.
                   "<script type=module src='" +
                   Module("throw.js", "throw new Error('boom')") +
                   "'></script>"
                   "<script type=module>await new Promise(r => setTimeout(r)); throw new Error('late')</script>"
                   // The modules of another type: refused.
                   "<script type=module>import data from '" +
                   Module("d.json", "{}", "&type=application/json") +
                   "' with { type: 'json' }</script>"
                   // An attribute of an unsupported key: a parse error.
                   "<script type=module>import '" +
                   Module("x.js", "export const x = 1") +
                   "' with { foo: 'bar' }</script>"
                   "<script type=module>log.push('next')</script>"));
  EXPECT_EQ(test::Log(page), R"(["load","next"])");
  // The timer of the await runs before the next script.
  const auto errors = page.uncaughtErrors();
  ASSERT_EQ(errors.size(), 6U);
  EXPECT_TRUE(errors[0].starts_with("SyntaxError")) << errors[0];
  EXPECT_TRUE(errors[1].starts_with("SyntaxError") && errors[1].contains("missing")) << errors[1];
  EXPECT_TRUE(errors[2].starts_with("Error: boom")) << errors[2];
  EXPECT_TRUE(errors[3].starts_with("Error: late")) << errors[3];
  EXPECT_TRUE(errors[4].starts_with("TypeError") && errors[4].contains("json")) << errors[4];
  EXPECT_TRUE(errors[5].starts_with("SyntaxError") && errors[5].contains("'foo'")) << errors[5];
}

// The imports of a module that fail: a bare specifier (reported), a URL other than http and https, a module that
// failed before (error events).
TEST_F(WebModuleTest, FailedImportsInsideModules) {
  WebClient client(Config());
  WebPage page = test::Navigate(
      client, Page("<script>log = []</script>"
                   "<script type=module onerror=\"log.push('bare')\">import 'react'</script>"
                   "<script type=module onerror=\"log.push('data')\">import 'data:text/javascript,0'</script>"
                   "<script type=module src='/m/missing.js?status=404' onerror=\"log.push('missing')\"></script>"
                   "<script type=module onerror=\"log.push('missing again')\">import '/m/missing.js?status=404'"
                   "</script><script type=module>log.push('after')</script>"));
  EXPECT_EQ(test::Log(page), R"(["data","missing","missing again","after"])");
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].starts_with("TypeError: Failed to resolve module specifier 'react'"))
      << page.uncaughtErrors()[0];
}

// The deadline passes while a graph loads: the next modules are not fetched, the page is returned as it is. The
// modules compiled before it fail for good, with an error that the scripts can catch (QuickJS-ng keeps them, with their
// import that did not load), and those not fetched yet load again.
TEST_F(WebModuleTest, DeadlineDuringAGraph) {
  WebClient client(Config());
  // Each level is answered after 350 ms: the deadline passes while the first one is fetched, before its import.
  WebPage page = test::Navigate(client,
                                Page("<script type=module>import { depth } from '/chain?n=2&delay=350';"
                                     "window.depth = depth</script>"),
                                {.timeout = std::chrono::milliseconds{300}});
  EXPECT_EQ(page.stopReason(), StopReason::TimedOut);
  EXPECT_EQ(test::Eval(page, "typeof depth"), R"("undefined")");
  EXPECT_TRUE(page.uncaughtErrors().empty()) << page.uncaughtErrors()[0];

  EXPECT_EQ(test::Eval(page,
                       "out = []; (async () => { for (const n of [2, 1]) { try {"
                       "out.push((await import('/chain?n=' + n + '&delay=350')).depth) } catch (e) {"
                       "out.push(e.name, e.message.includes('deadline')) } } })(); 0"),
            "0");
  EXPECT_EQ(test::Eval(page, "out"), R"(["TypeError",true,1])");
}

// A page freed while its modules await, the awaited promise held by a global, a module variable, or a module loaded by
// import(): QuickJS-ng up to 0.18.0 then wrote into the freed modules (see AERONET_QJS_MODULES).
TEST_F(WebModuleTest, PagesFreedWhileModulesAwait) {
  WebClient client(Config());
  const std::string pending = Module("pending.js", "const p = new Promise(() => {}); await p; export const x = 1;");
  const std::string imported =
      "<script>import('" + pending + "')</script><script type=module>import '" + pending + "'</script>";
  for (std::string html : {
           std::string("<script type=module>globalThis.p = new Promise(() => {}); await globalThis.p</script>"),
           std::string("<script type=module>const p = new Promise(() => {}); await p</script>"),
           imported,
       }) {
    html += "<script>window.loaded = true</script>";
    WebPage page = test::Navigate(client, Page(html));
    EXPECT_EQ(test::Eval(page, "loaded"), "true");
    EXPECT_TRUE(page.uncaughtErrors().empty()) << page.uncaughtErrors()[0];
  }
}

TEST_F(WebModuleTest, ModulesRunOnce) {
  WebClient client(Config());
  const std::string counter = Module("counter.js", "window.runs = (window.runs || 0) + 1; export default runs");
  EXPECT_EQ(LogOf(client, "<script>log = []</script><script type=module src='" + counter +
                              "'></script><script type=module src='" + counter +
                              "'></script><script type=module>import n from '" + counter +
                              "'; log.push(n, runs, (await import('" + counter + "')).default)</script>"),
            "[1,1,1]");
}

TEST_F(WebModuleTest, Cycles) {
  WebClient client(Config());
  EXPECT_EQ(LogOf(client, "<script>log = []</script><script type=module src='/static/a.js'></script>"), R"(["b","a"])");
}

// A cycle whose root imports a missing module: the graph fails, its modules stay loaded, never evaluated, and a later
// import of the other module of the cycle fails without running anything.
TEST_F(WebModuleTest, CycleWithAFailedImport) {
  WebClient client(Config());
  WebPage page =
      test::Navigate(client, Page("<script>log = []</script>"
                                  "<script type=module src='/static/c.js' onerror=\"log.push('error')\"></script>"
                                  "<script type=module>await import('/static/d.js').catch(e => log.push(e.name));"
                                  "await import('/static/c.js').catch(e => log.push(e.name))</script>"));
  EXPECT_EQ(test::Log(page), R"(["error","TypeError","TypeError"])");
}

// A module that reaches a failed one through a cycle completes first, without failure of its own: it fails with the
// error of the graph too, so that a later import of it, or of a module that imports it, runs nothing. A module of the
// graph that does not reach the failure loads.
TEST_F(WebModuleTest, ModulesReachingAFailureThroughACycle) {
  WebClient client(Config());
  WebPage page =
      test::Navigate(client, Page("<script>log = []</script>"
                                  "<script type=module src='/static/e.js' onerror=\"log.push('error')\"></script>"));
  EXPECT_EQ(test::Log(page), R"(["error"])");
  EXPECT_EQ(test::Eval(page,
                       "(async () => { for (const name of ['f', 'g', 'y']) { try {"
                       "await import('/static/' + name + '.js'); log.push(name + ' loaded') } catch (e) {"
                       "log.push(e.message.includes('Failed to fetch module') && e.message.includes('missing.js')) } }"
                       "})(); 0"),
            "0");
  EXPECT_EQ(test::Log(page), R"(["error",true,true,"y","y loaded"])");
  EXPECT_TRUE(page.uncaughtErrors().empty()) << page.uncaughtErrors()[0];

  WebPage second =
      test::Navigate(client, Page("<script>log = []</script>"
                                  "<script type=module src='/static/h.js' onerror=\"log.push('error')\"></script>"));
  EXPECT_EQ(test::Eval(second,
                       "(async () => { for (const name of ['j', 'k']) { try {"
                       "await import('/static/' + name + '.js'); log.push(name + ' loaded') } catch (e) {"
                       "log.push(e.message.includes('missing.js')) } } })(); 0"),
            "0");
  EXPECT_EQ(test::Log(second), R"(["error",true,"k","k loaded"])");
  EXPECT_TRUE(second.uncaughtErrors().empty()) << second.uncaughtErrors()[0];
}

TEST_F(WebModuleTest, CorsModeOfTheModuleRequests) {
  WebClient client(Config());
  const std::string origin = test::ServerUrl(gServer);
  const std::string other = test::ServerUrl(gOther);
  WebPage page = test::Navigate(
      client, Page("<script>log = []; document.cookie = 'c=1'</script>"
                   // Of the document: its cookies, no Origin.
                   "<script type=module>import { origin, cookie } from '" +
                   Module("same.js", "", "&echo=1") +
                   "'; log.push(origin, cookie)</script>"
                   // Of another origin: allowed by its server, with an Origin, its cookies only with use-credentials.
                   "<script type=module>import { origin, cookie } from '" +
                   other + Module("cors.js", "", "&echo=1&acao=*") +
                   "'; log.push(origin, cookie)</script>"
                   "<script type=module crossorigin=use-credentials>import { cookie } from '" +
                   other + Module("credentials.js", "", "&echo=1&acao=origin&acac=1") +
                   "'; log.push(cookie)</script>"
                   // Not allowed by its server.
                   "<script type=module onerror=\"log.push('denied')\">import '" +
                   other + Module("denied.js", "") + "'</script>"));
  EXPECT_EQ(test::Log(page), "[\"\",\"c=1\",\"" + origin + "\",\"\",\"c=1\",\"denied\"]");
  EXPECT_TRUE(page.uncaughtErrors().empty()) << page.uncaughtErrors()[0];
}

// The Referer of a module request: from the base URL of the importing module, from the document for a module script
// and the imports of an inline one, with the default policy of the browsers (strict-origin-when-cross-origin).
TEST_F(WebModuleTest, RefererOfTheModuleRequests) {
  WebClient client(Config());
  const std::string other = test::ServerUrl(gOther);
  // A module of another origin importing one of its own origin: its full URL.
  const std::string importer = other + Module("importer.js",
                                              "import { referer } from './nested.js?src=&echo=1&acao=*';"
                                              "log.push(referer === import.meta.url)",
                                              "&acao=*");
  // A module of the document origin importing one of another origin: its origin.
  const std::string crossing = Module(
      "crossing.js", "import { referer } from '" + other + "/m/target.js?src=&echo=1&acao=*'; log.push(referer)");
  WebPage page = test::Navigate(
      client, Page("<script>log = []</script><script type=module src='" +
                   Module("root.js", "queueMicrotask(() => log.push(referer === location.href));", "&echo=1") +
                   "'></script><script type=module>import { referer } from '" + Module("inline.js", "", "&echo=1") +
                   "'; log.push(referer === location.href)</script><script type=module src='" + importer +
                   "'></script><script type=module src='" + crossing + "'></script>"));
  EXPECT_EQ(test::Log(page), "[true,true,true,\"" + test::ServerUrl(gServer) + "/\"]");
  EXPECT_TRUE(page.uncaughtErrors().empty()) << page.uncaughtErrors()[0];
}

TEST_F(WebModuleTest, RedirectedModules) {
  WebClient client(Config());
  const std::string target = Module("real/a.js", "import { b } from './b.js" + Src("export const b = 'b'") +
                                                     "'; window.runs = (window.runs || 0) + 1;"
                                                     "log.push(b, import.meta.url.includes('/m/real/a.js'))");
  const std::string redirect = "/m/redirect.js?to=" + test::PercentEncode(target);
  // Its imports resolve against the URL of its response, it runs once for its URL.
  EXPECT_EQ(
      LogOf(client, "<script>log = []</script><script type=module src='" + redirect +
                        "'></script><script type=module>await import('" + redirect + "'); log.push(runs)</script>"),
      R"(["b",true,1])");
}

TEST_F(WebModuleTest, ModuleScriptsInsertedByScripts) {
  WebClient client(Config());
  const std::string external = Module("inserted.js", "log.push('external', document.currentScript)");
  EXPECT_EQ(LogOf(client,
                  "<script>log = [];"
                  "const ext = document.createElement('script'); ext.type = 'module'; ext.src = '" +
                      external +
                      "'; ext.onload = () => log.push('load'); document.head.append(ext);"
                      "const inline = document.createElement('script'); inline.type = 'module';"
                      "inline.text = \"log.push('inline')\"; document.head.append(inline);"
                      // Its code is read when it is inserted.
                      "inline.text = \"log.push('changed')\";"
                      "const legacy = document.createElement('script'); legacy.noModule = true;"
                      "legacy.text = \"log.push('nomodule')\"; document.head.append(legacy);"
                      "log.push('end')</script>"),
            R"(["end","external",null,"load","inline"])");
}

// An inserted script waits for its task with a copy of its code or URL, counted in the native memory of the page: a
// loop inserting module scripts fails at the limit, and those queued before run.
TEST_F(WebModuleTest, InsertedModuleScriptsCountInTheMemoryOfThePage) {
  WebClientConfig config = Config();
  config.maxScriptMemory = std::size_t{16} << 20U;
  WebClient client(config);
  // The text node of 256 KiB moves from script to script: only the elements are new nodes.
  WebPage page = test::Navigate(
      client,
      Page("<script>window.queued = 0;"
           "const code = document.createTextNode('window.runs = (window.runs || 0) + 1 //' + 'x'.repeat(1 << 18));"
           "for (let i = 0; i < 1000; ++i) { const s = document.createElement('script'); s.type = 'module';"
           "document.head.append(s); s.append(code); s.remove(); ++queued }</script>"));
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].contains("out of memory")) << page.uncaughtErrors()[0];
  const int queued = std::stoi(test::Eval(page, "queued"));
  EXPECT_GT(queued, 0);
  EXPECT_LT(queued, 64);  // 16 MiB / 256 KiB
  EXPECT_EQ(test::Eval(page, "runs"), std::to_string(queued));
}

TEST_F(WebModuleTest, ModulesCountTowardTheLimitsOfTheDocument) {
  WebClientConfig config = Config();
  config.maxRequests = 2;
  WebClient client(config);
  // import() requests 3 modules, the module scripts of the parser are not counted. (An empty body would have no
  // Content-Type: not a module.)
  const std::string graph = Module("limit.js", "import './a.js" + Src("0") + "'; import './b.js" + Src("0") + "'");
  EXPECT_EQ(LogOf(client, "<script>log = []</script><script type=module src='" + Module("top.js", "0") +
                              "' onload=\"log.push('load')\"></script><script type=module>"
                              "await import('" +
                              graph + "').catch(e => log.push(e.message.includes('maxRequests')))</script>"),
            R"(["load",true])");

  // The code of evaluate() is not counted, that of the modules it imports is.
  config = Config();
  config.maxScriptBytes = 64;
  WebClient limited(config);
  WebPage page =
      test::Navigate(limited, Page("<script type=module>window.inline = 1; //" + std::string(64, 'x') + "</script>"));
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].contains("inline module")) << page.uncaughtErrors()[0];
  EXPECT_EQ(test::Eval(page, "typeof inline"), R"("undefined")");
  EXPECT_EQ(test::Eval(page, "import('" + Module("large.js", "window.x = 1; //" + std::string(64, 'x')) +
                                 "').catch(e => { window.error = e.name + ': ' + e.message }); 0"),
            "0");
  EXPECT_TRUE(test::Eval(page, "error").starts_with("\"RangeError")) << test::Eval(page, "error");
  EXPECT_TRUE(test::Eval(page, "error").contains("maxScriptBytes")) << test::Eval(page, "error");
  EXPECT_EQ(test::Eval(page, "typeof x"), R"("undefined")");
}

// The large modules are compiled once per client: the next pages read their bytecode back, imports resolved again. A
// script of the same URL and code as a classic script has its own entry.
TEST_F(WebModuleTest, ModulesAreReadBackFromTheCache) {
  WebClient client(Config());
  const std::string padding = "//" + std::string(internal::ScriptCache::kMinScriptSize, 'p') + "\n";
  const std::string code = padding + "log.push(this === undefined ? 'module' : 'classic');";
  const std::string dependency = padding + "export const value = 7;";
  const std::string html =
      "<script>log = []</script><script src='" + Module("both.js", code) + "'></script><script type=module src='" +
      Module("both.js", code) + "'></script><script type=module>" + padding + "import { value } from '" +
      Module("dep.js", dependency) + "'; log.push(value, import.meta.url === location.href)</script>";
  const std::string expected = R"(["classic","module",7,true])";
  EXPECT_EQ(LogOf(client, html), expected);
  EXPECT_EQ(LogOf(client, html), expected);
}

// The read of a cached module resolves its imports, which compile and store their own bytecode: the entry being read
// stays until the read ends, even beyond the size limit of the cache.
TEST_F(WebModuleTest, CachedModuleReadWhileItsImportsAreStored) {
  WebClientConfig config = Config();
  // Each bytecode fits alone, not both: the first page keeps the root only, the second one reads it back, and the store
  // of its import, compiled again, drops it as the least recently used entry.
  config.maxScriptCacheBytes = kLargeLiteralSize * 3 / 2;
  WebClient client(config);
  const std::string expected = "[" + std::to_string(kLargeLiteralSize) + ',' + std::to_string(kLargeLiteralSize) + ']';
  for (int load = 0; load < 3; ++load) {
    EXPECT_EQ(LogOf(client, "<script>log = []</script><script type=module src='/large/root.js'></script>"), expected);
  }
}

TEST_F(WebModuleTest, DeepImportChains) {
  WebClient client(Config());
  EXPECT_EQ(LogOf(client,
                  "<script>log = []</script><script type=module>import { depth } from '/chain?n=30';"
                  "log.push(depth)</script>"),
            "[30]");

  // Each level of a graph nests a compilation: a too deep one fails at the stack limit, and the page goes on.
  WebClientConfig config = Config();
  config.maxScriptStackSize = std::size_t{256} << 10U;
  config.maxRequests = 100000;
  WebClient shallow(config);
  WebPage page = test::Navigate(shallow, Page("<script type=module>import '/chain?n=50000'</script>"
                                              "<script type=module>window.after = 1</script>"));
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].starts_with("RangeError")) << page.uncaughtErrors()[0];
  EXPECT_EQ(test::Eval(page, "after"), "1");
}

// Each step of a module graph fails in turn as the memory limit grows: no value leaks (QuickJS asserts that no object
// is left when a runtime is freed), no freed module is reached, and the scripts complete once the limit is enough.
TEST_F(WebModuleTest, MemoryLimitDuringModuleGraphs) {
  static constexpr std::size_t kStep = 1024;
  const std::string html =
      "<script type=module>import { a } from '/filler/a.js'; const { default: d } = await import('/filler/d.js');"
      "await import('/m/mem/missing.js?status=404').catch(() => {}); await import('/static/d.js').catch(() => {});"
      "window.out = a + d</script>";
  const std::string url = Page(html);
  const auto completes = [&url](std::size_t limit) {
    WebClientConfig config = Config();
    config.maxScriptMemory = limit;
    WebClient client(config);
    try {
      auto page = client.navigate(url);
      return page && page->evaluate("out").value_or("") == "7" ? 1 : 0;
    } catch (const std::bad_alloc&) {
      return -1;
    } catch (const std::runtime_error&) {  // too little memory for the setup of the page
      return -1;
    }
  };
  // The setup of a page takes most of the memory: the limit grows fast until a page sets up, then by small steps.
  std::size_t limit = std::size_t{8} << 10U;
  for (std::size_t next = limit; completes(next) < 0; next += next / 16) {
    ASSERT_LT(next, std::size_t{64} << 20U);
    limit = next;
  }
  int failures = 0;
  for (; completes(limit) <= 0; limit += kStep) {
    ASSERT_LT(limit, std::size_t{64} << 20U);
    ++failures;
  }
  EXPECT_GT(failures, 100);
}

// Each allocation of the module loading fails in turn, in a page that loads a cycle, a graph failing through a missing
// import, import.meta.resolve(), import() of a failed graph and of a JSON module: the page goes on, until a limit lets
// all of it complete.
TEST_F(WebModuleTest, MemoryLimitDuringModules) {
  const std::string url = Page(
      "<script>log = [];</script><script type=module src=/static/a.js></script>"
      "<script type=module>import '/static/c.js';</script><script type=module>"
      "const resolved = import.meta.resolve('./x.js');"
      "const failed = await import('/static/h.js').catch(e => e.name);"
      "const json = await import('/static/k.js', { with: { type: 'json' } }).catch(e => e.name);"
      "window.out = [resolved.endsWith('/x.js'), failed, json].join();</script>");
  const auto completes = [&url](std::size_t limit) {
    WebClientConfig config = Config();
    config.maxScriptMemory = limit;
    WebClient client(config);
    try {
      auto page = client.navigate(url, {.until = LoadState::NetworkIdle});
      return page && page->evaluate("out").value_or("") == R"("true,TypeError,TypeError")" ? 1 : 0;
    } catch (const std::bad_alloc&) {
      return -1;
    } catch (const std::runtime_error&) {  // too little memory for the setup of the page
      return -1;
    }
  };
  std::size_t limit = std::size_t{128} << 10U;
  for (std::size_t next = limit; completes(next) < 0; next += next / 16) {
    ASSERT_LT(next, std::size_t{64} << 20U);
    limit = next;
  }
  const std::size_t setupLimit = limit;
  int failures = 0;
  for (; completes(limit) <= 0; limit += 256) {
    ASSERT_LT(limit, setupLimit + (std::size_t{4} << 20U)) << "the modules never complete";
    ++failures;
  }
  EXPECT_GT(failures, 10);
}

// A memory exhaustion inside QuickJS-ng while it resolves the imports of a module (many small modules: what the graph
// keeps grows past the memory left after the setup) stops the loading of modules in the page for good: the next module
// script fails with an error event, and the page goes on.
TEST_F(WebModuleTest, MemoryExhaustionDuringResolutions) {
  std::string html = "<script type=module>";
  for (int module = 0; module < 30; ++module) {
    html += "import { v as v" + std::to_string(module) + " } from '/tiny?k=" + std::to_string(module) + "';";
  }
  html += "window.out = v29</script><script type=module onerror='window.broken = 1'>window.after = 1</script>";
  const std::string url = Page(html);
  int broken = 0;
  int failedImports = 0;
  const auto completes = [&url, &broken, &failedImports](std::size_t limit) {
    WebClientConfig config = Config();
    config.maxScriptMemory = limit;
    WebClient client(config);
    try {
      auto page = client.navigate(url);
      if (page && page->evaluate("typeof broken").value_or("") == R"("number")") {
        ++broken;
        // An import() fails at once too, when it has the memory to report it.
        if (page->evaluate("import('/tiny?k=0').catch(e => { window.importError = e.message; }); 0") &&
            page->evaluate("importError").value_or("").contains("no module loads")) {
          ++failedImports;
        }
      }
      return page && page->evaluate("out").value_or("") == "29" ? 1 : 0;
    } catch (const std::bad_alloc&) {
      return -1;
    } catch (const std::runtime_error&) {  // too little memory for the setup of the page
      return -1;
    }
  };
  std::size_t limit = std::size_t{8} << 10U;
  for (std::size_t next = limit; completes(next) < 0; next += next / 16) {
    ASSERT_LT(next, std::size_t{64} << 20U);
    limit = next;
  }
  for (; completes(limit) <= 0; limit += 128) {
    ASSERT_LT(limit, std::size_t{64} << 20U);
  }
  EXPECT_GT(broken, 0);
  EXPECT_GT(failedImports, 0);
}

}  // namespace
}  // namespace aeronet
