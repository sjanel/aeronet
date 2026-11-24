// The page loop of WebClient against local servers: script order and types, events, document.write(), timers on the
// virtual clock, the stop conditions (load states, selectors, timeout), navigations (location, meta refresh, limits),
// cookies across sites, storage, history, location and navigator.
#include "aeronet/web-client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../src/js-module.hpp"
#include "../src/web-url.hpp"
#include "aeronet/cookie-jar.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/web-client-config.hpp"
#include "aeronet/web-page.hpp"
#include "web-test-util.hpp"

namespace aeronet {
namespace {

using std::chrono::milliseconds;

std::string StoreUrl(std::string_view path);
std::string QueueUrl(std::string_view path);

HttpResponse Redirect(const HttpRequestView& req, std::string_view location) {
  HttpResponse resp = req.makeResponse(http::StatusCodeFound);
  resp.location(location);
  return resp;
}

test::TestServer MakeServer() {
  test::TestServer server = test::MakePageServer();
  auto router = server.router();
  // GET /typed-js?type=<Content-Type>&src=<bytes>: a script of any encoding.
  router.setPath(http::Method::GET, "/typed-js", [](const HttpRequestView& req) {
    return req.makeResponse(req.queryParamValue("src").value_or(""), req.queryParamValue("type").value_or(""));
  });
  router.setPath(http::Method::GET, "/sub/base.js", [](const HttpRequestView& req) {
    return req.makeResponse("window.fromBase = true", "text/javascript");
  });
  router.setPath(http::Method::GET, "/missing", [](const HttpRequestView& req) {
    return req.makeResponse(http::StatusCodeNotFound, "<script>window.ran = true</script>", http::ContentTypeTextHtml);
  });
  router.setPath(http::Method::GET, "/redirect",
                 [](const HttpRequestView& req) { return Redirect(req, req.queryParamValue("to").value_or("/")); });
  router.setPath(http::Method::GET, "/echo-cookies", [](const HttpRequestView& req) {
    return req.makeResponse("<p id=cookies>" + std::string(req.headerValueOrEmpty(http::Cookie)) + "</p>",
                            http::ContentTypeTextHtml);
  });
  router.setPath(http::Method::GET, "/cookies", [](const HttpRequestView& req) {
    return req
        .makeResponse(
            "<script>window.seen = document.cookie; document.cookie = 'secret=stolen'; document.cookie = 'h=1; "
            "HttpOnly'; document.cookie = 'mine=1; Path=/'; window.after = document.cookie;</script>",
            http::ContentTypeTextHtml)
        .headerAddLine(http::SetCookie, "secret=1; Path=/; HttpOnly")
        .headerAddLine(http::SetCookie, "visible=1; Path=/");
  });
  // The shop of a Queue-it like flow: without its token cookie, a visitor goes through the waiting room.
  router.setPath(http::Method::GET, "/product", [](const HttpRequestView& req) {
    const std::string_view cookies = req.headerValueOrEmpty(http::Cookie);
    if (req.queryParamValue("queueittoken").value_or("") == "ok" || cookies.contains("queueittoken=ok")) {
      return req
          .makeResponse(
              "<script id=\"__NEXT_DATA__\" type=\"application/json\">{\"stock\":\"OutOfStock\"}</script>"
              "<script>window.productScriptRan = true</script>",
              http::ContentTypeTextHtml)
          .headerAddLine(http::SetCookie, "queueittoken=ok; Path=/");
    }
    return Redirect(req, QueueUrl("/queue?target=" + test::PercentEncode(StoreUrl("/product"))));
  });
  return server;
}

// The waiting room, another site (localhost): its page checks the cookies with a script, then navigates.
test::TestServer MakeQueue() {
  test::TestServer server = test::MakePageServer();
  auto router = server.router();
  router.setPath(http::Method::GET, "/queue", [](const HttpRequestView& req) {
    return req.makeResponse(
        "<noscript>The site requires JavaScript</noscript><script>"
        "if (navigator.cookieEnabled) { document.cookie = 'qtest=1; Path=/';"
        "if (document.cookie.includes('qtest=1')) { document.location.href = '/pass' + location.search; } }"
        "</script>",
        http::ContentTypeTextHtml);
  });
  router.setPath(http::Method::GET, "/pass", [](const HttpRequestView& req) {
    if (!req.headerValueOrEmpty(http::Cookie).contains("qtest=1")) {
      return req.makeResponse(http::StatusCodeForbidden);
    }
    return Redirect(req, std::string(req.queryParamValue("target").value_or("")) + "?queueittoken=ok");
  });
  return server;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gServer = MakeServer();
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gQueue = MakeQueue();

std::string StoreUrl(std::string_view path) { return test::ServerUrl(gServer) + std::string(path); }

std::string QueueUrl(std::string_view path) { return test::ServerUrl(gQueue, "localhost") + std::string(path); }

std::string Page(std::string_view html) { return test::PageUrl(gServer, html); }

// The relative URL of a page, for the navigations of the scripts.
std::string PagePath(std::string_view html) { return "/html?src=" + test::PercentEncode(html); }

std::string ScriptUrl(std::string_view code) { return StoreUrl(test::ScriptPath(code)); }

std::string Script(std::string_view code, std::string_view attributes = {}) {
  return "<script src=\"" + ScriptUrl(code) + "\" " + std::string(attributes) + "></script>";
}

// The URL of a closed port.
std::string ClosedPortUrl() {
  test::TestServer server = test::MakePageServer();
  std::string url = test::ServerUrl(server) + "/html";
  server.stop();
  return url;
}

using test::Log;

constexpr std::string_view kLogEvents =
    "<script>log = ['inline ' + document.readyState];"
    "document.addEventListener('readystatechange', () => log.push('readystatechange ' + document.readyState));"
    "document.addEventListener('DOMContentLoaded', () => log.push('DOMContentLoaded'));"
    "window.addEventListener('load', () => log.push('load ' + document.readyState));</script>";

std::string OrderPage() {
  return std::string(kLogEvents) + Script("log.push('async')", "async") +
         Script("log.push('defer ' + document.readyState)", "defer") +
         Script(
             "log.push('external ' + document.currentScript.id + ' ' + "
             "document.currentScript.src.includes('/js?src='))",
             "id=ext") +
         "<script id=in2>log.push('inline2 ' + document.currentScript.id)</script>";
}

TEST(WebClientTest, ScriptsRunInTheOrderOfABrowser) {
  WebClient client;
  WebPage page = test::Navigate(client, Page(OrderPage()));
  EXPECT_EQ(page.stopReason(), StopReason::StateReached);
  EXPECT_EQ(Log(page),
            R"(["inline loading","external ext true","inline2 in2","readystatechange interactive",)"
            R"("defer interactive","DOMContentLoaded","async","readystatechange complete","load complete"])");
  EXPECT_EQ(test::Eval(page, "document.currentScript"), "null");
  EXPECT_TRUE(page.uncaughtErrors().empty());
}

TEST(WebClientTest, DomContentLoadedStopsBeforeTheAsyncScripts) {
  WebClient client;
  WebPage page = test::Navigate(client, Page(OrderPage()), {.until = LoadState::DomContentLoaded});
  EXPECT_EQ(Log(page), R"(["inline loading","external ext true","inline2 in2","readystatechange interactive",)"
                       R"("defer interactive","DOMContentLoaded"])");
  EXPECT_EQ(test::Eval(page, "document.readyState"), R"("interactive")");
}

// The classic scripts and the module scripts run (deferred: after the others, see web-module_test.cpp), not the data
// blocks, nor the nomodule scripts, fallbacks of the browsers without modules.
TEST(WebClientTest, ScriptTypes) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script>log = []</script>"
                   "<script type=module>log.push('module')</script><script nomodule>log.push('nomodule')</script>"
                   "<script type='application/json'>log.push('json')</script><script type=''>log.push('empty')</script>"
                   "<script type=' TEXT/JavaScript '>log.push('case')</script>"
                   "<script type='text/javascript; charset=utf-8'>log.push('parameters')</script>"
                   "<script language=javascript>log.push('language')</script>"
                   "<script language=vbscript>log.push('vbscript')</script><script language=''>log.push('no "
                   "language')</script><script src=''></script><script></script>"));
  // Without module support, as in a browser without modules: the nomodule scripts run, not the module ones.
  EXPECT_EQ(Log(page), internal::ModulesEnabled() ? R"(["empty","case","language","no language","module"])"
                                                  : R"(["nomodule","empty","case","language","no language"])");
}

TEST(WebClientTest, ExternalScriptErrors) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script>log = []</script>"
                   "<script src='/missing' onerror=\"log.push('404 ' + event.type)\"></script>"
                   "<script src='" +
                   ClosedPortUrl() +
                   "' onerror=\"log.push('closed')\"></script>"
                   "<script src='data:text/javascript,log.push(1)' onerror=\"log.push('data')\"></script>"
                   "<script src='http://[bad' onerror=\"log.push('invalid')\"></script>"
                   // Not the URL of the document: no request.
                   "<script src='' onerror=\"log.push('empty')\"></script>" +
                   Script("log.push('ran')", "onload=\"log.push('loaded')\"")));
  EXPECT_EQ(Log(page), R"(["404 error","closed","data","invalid","empty","ran","loaded"])");
  EXPECT_TRUE(page.uncaughtErrors().empty());
}

// Decoded as browsers do: byte order mark, charset of the response, charset attribute, encoding of the document.
TEST(WebClientTest, ScriptsAreDecodedByTheirEncoding) {
  const auto typed = [](std::string_view code, std::string_view type, std::string_view attributes = {}) {
    return "<script " + std::string(attributes) + " src='" +
           StoreUrl("/typed-js?type=" + test::PercentEncode(type) + "&src=" + test::PercentEncode(code)) +
           "'></script>";
  };
  WebClient client;
  WebPage page = test::Navigate(
      client, Page(typed("log = ['\xE9']", "text/javascript; charset=windows-1252", "charset=iso-8859-2") +
                   typed("log.push('\xA3')", "text/javascript", "charset=iso-8859-2") +
                   typed("log.push('a\xFF"
                         "b')",
                         "text/javascript") +
                   typed("\xEF\xBB\xBFlog.push('\xC3\xA9 bom')", "text/javascript; charset=windows-1252")));
  EXPECT_EQ(Log(page),
            "[\"\xC3\xA9\",\"\xC5\x81\",\"a\xEF\xBF\xBD"
            "b\",\"\xC3\xA9 bom\"]");
  EXPECT_TRUE(page.uncaughtErrors().empty());
  // The encoding of the document, for a script without charset.
  WebPage legacy =
      test::Navigate(client, Page("<meta charset=windows-1252>" + typed("log = ['\xA3']", "text/javascript")));
  EXPECT_EQ(Log(legacy), "[\"\xC2\xA3\"]");
}

// The window is not on the path of the load events of the elements (DOM "get the parent" of a Document).
TEST(WebClientTest, LoadEventsOfScriptsDoNotReachTheWindow) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script>log = []; window.addEventListener('load', e => log.push('window ' + (e.target === window)),"
                   "true); document.addEventListener('load', e => log.push('document ' + e.target.nodeName), true)"
                   "</script>" +
                   Script("1")));
  EXPECT_EQ(Log(page), R"(["document SCRIPT","window true"])");
}

TEST(WebClientTest, ScriptBytesLimit) {
  WebClientConfig config;
  config.maxScriptBytes = 30;
  WebClient client(config);
  WebPage page =
      test::Navigate(client, Page("<script>log = [1]</script>" + Script("log.push('external, too long')") +
                                  "<script>log.push(2)</script><script>log.push('inline, too long')</script>"));
  EXPECT_EQ(Log(page), "[1,2]");
  ASSERT_EQ(page.uncaughtErrors().size(), 2U);
  EXPECT_TRUE(page.uncaughtErrors()[0].contains("/js?src=")) << page.uncaughtErrors()[0];
  EXPECT_TRUE(page.uncaughtErrors()[1].starts_with("aeronet: script inline not run")) << page.uncaughtErrors()[1];

  config.maxScriptBytes = 0;
  WebClient noScripts(config);
  WebPage unscripted = test::Navigate(noScripts, Page("<script>window.ran = 1</script>"));
  EXPECT_EQ(test::Eval(unscripted, "window.ran"), "");
}

// The stack of an error of an external script names its URL, even after the script resolved another URL.
TEST(WebClientTest, StackOfAnExternalScriptNamesItsUrl) {
  constexpr std::string_view kCode =
      "const a = document.createElement('a'); a.href = '/other'; log = [a.href.endsWith('/other')];"
      "throw new Error('external')";
  WebClient client;
  WebPage page = test::Navigate(client, Page(Script(kCode)));
  EXPECT_EQ(Log(page), "[true]");
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].starts_with("Error: external\n")) << page.uncaughtErrors()[0];
  EXPECT_TRUE(page.uncaughtErrors()[0].contains(ScriptUrl(kCode))) << page.uncaughtErrors()[0];
}

TEST(WebClientTest, DocumentWrite) {
  WebClient client;
  WebPage page = test::Navigate(
      client,
      Page("<script>log = []; document.write('<p id=w>wr', 'itten</p><script>log.push(\"written\")<\\/script>');"
           "document.writeln('<i', '>'); log.push('writer ' + document.getElementById('w'))</script>"
           "<script>log.push('next ' + document.getElementById('w').textContent + ' ' + "
           "document.querySelector('i').textContent.length)</script>"
           "<p><script>document.currentScript.remove(); document.write('<b id=dropped>')</script></p>"
           "<script>setTimeout(() => document.write('<u id=late>'))</script>"));
  EXPECT_EQ(Log(page), R"(["writer null","written","next written 1"])");
  EXPECT_EQ(test::Eval(page, "[document.getElementById('dropped'), document.getElementById('late')]"), "[null,null]");
}

TEST(WebClientTest, MicrotasksRunAfterEachScript) {
  WebClient client;
  WebPage page =
      test::Navigate(client, Page("<script>log = []; Promise.resolve().then(() => log.push('microtask'))</script>"
                                  "<script>log.push('second')</script>"));
  EXPECT_EQ(Log(page), R"(["microtask","second"])");
}

constexpr std::string_view kTimers =
    "<script>log = [];"
    "setTimeout(() => log.push('t10'), 10); setTimeout(() => log.push('t0'), 0);"
    "setTimeout((a, b) => log.push('args ' + a + b), -5, 1, 2);"
    "clearTimeout(setTimeout(() => log.push('cleared'), 0));"
    "setTimeout(\"log.push('string')\", 0); setTimeout(() => { throw new Error('in a timer') });"
    "requestAnimationFrame(time => log.push('frame ' + typeof time));"
    "requestIdleCallback(deadline => log.push('idle ' + deadline.timeRemaining()));"
    "cancelAnimationFrame(requestAnimationFrame(() => log.push('cancelled frame')));"
    "cancelIdleCallback(requestIdleCallback(() => log.push('cancelled idle')));"
    "window.addEventListener('load', () => log.push('load'));</script>";

TEST(WebClientTest, TimersDueAtOnceRunWhileTheDocumentLoads) {
  WebClient client;
  WebPage page = test::Navigate(client, Page(kTimers));
  EXPECT_EQ(Log(page), R"(["t0","args 12","string","load"])");
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].starts_with("Error: in a timer"));
}

TEST(WebClientTest, NetworkIdleRunsTheTimersOnAVirtualClock) {
  WebClient client;
  const auto start = std::chrono::steady_clock::now();
  WebPage page = test::Navigate(
      client, Page(std::string(kTimers) + "<script>setTimeout(() => log.push('a minute later'), 59000)</script>"),
      {.until = LoadState::NetworkIdle});
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{5});
  EXPECT_EQ(Log(page), R"(["t0","args 12","string","load","idle 50","t10","frame number","a minute later"])");
}

TEST(WebClientTest, TimersBeyondTheHorizonNeverRun) {
  WebClientConfig config;
  config.maxTimerDelay = milliseconds{1000};
  WebClient client(config);
  WebPage page = test::Navigate(
      client,
      Page("<script>log = []; setTimeout(() => log.push('late'), 1001); setTimeout(() => log.push('in'), 1000);"
           "(function chain(n) { log.push(n); setTimeout(() => chain(n + 1), 400); })(0);"
           "setInterval(() => log.push('interval'), 600); setInterval(() => log.push('never'), 2000)"
           "</script>"),
      {.until = LoadState::NetworkIdle});
  // 0, then at 400 ms 1, 600 interval, 800 2, 1000 in: the timers due at 1200 and later never run.
  EXPECT_EQ(Log(page), R"([0,1,"interval",2,"in"])");
}

TEST(WebClientTest, Intervals) {
  WebClientConfig config;
  config.maxIntervalRuns = 3;
  WebClient client(config);
  WebPage page = test::Navigate(
      client,
      Page("<script>log = []; setInterval(n => log.push('capped ' + n), 10, 1);"
           "let count = 0; const id = setInterval(() => { if (++count === 2) clearInterval(id); log.push(count) "
           "}, 15); setInterval(\"log.push('string')\", 100); clearInterval(); clearTimeout('x')</script>"),
      {.until = LoadState::NetworkIdle});
  // At 30 ms, the interval rescheduled at 15 ms runs before the one rescheduled at 20 ms.
  EXPECT_EQ(Log(page), R"(["capped 1",1,"capped 1",2,"capped 1","string","string","string"])");

  config.maxIntervalRuns = 0;
  WebClient zeroIntervals(config);
  WebPage zeroPage =
      test::Navigate(zeroIntervals, Page("<script>log = []; setInterval(() => log.push('interval'), 1);</script>"),
                     {.until = LoadState::NetworkIdle});
  EXPECT_EQ(Log(zeroPage), "[]");
}

TEST(WebClientTest, NestedTimersAreClamped) {
  constexpr std::string_view kChain =
      "<script>count = 0; (function chain() { if (++count < 10) setTimeout(chain, 0); })();"
      "setInterval(() => ++intervals, 0); intervals = 0</script>";
  WebClient client;
  // Beyond 5 nested levels, a timer waits 4 ms: the virtual clock has to advance.
  WebPage loaded = test::Navigate(client, Page(kChain));
  EXPECT_EQ(test::Eval(loaded, "[count, intervals]"), "[6,5]");
  WebPage idle = test::Navigate(client, Page(kChain), {.until = LoadState::NetworkIdle});
  EXPECT_EQ(test::Eval(idle, "[count, intervals]"), "[10,100]");
}

// A zero-delay timer set by the microtasks of a timer is nested too: a promise-based yield loop lets the virtual clock
// advance to the later timers.
TEST(WebClientTest, ZeroDelayTimersSetByMicrotasksAreClamped) {
  WebClient client;
  WebPage page = test::Navigate(
      client,
      Page("<script>setTimeout(() => { const list = document.createElement('ul'); list.id = 'list';"
           "document.body.append(list) }, 50); (async () => { let yields = 0;"
           "while (!document.querySelector('#list')) { await new Promise(resolve => setTimeout(resolve)); ++yields }"
           "window.yields = yields })()</script>"),
      {.until = LoadState::NetworkIdle, .timeout = std::chrono::seconds{5}});
  EXPECT_EQ(page.stopReason(), StopReason::StateReached);
  // 5 levels at 0 ms, then 4 ms each: the 18th yield, at 52 ms, is the first one after the timer of 50 ms.
  EXPECT_EQ(test::Eval(page, "yields"), "18");
}

// The cleared timers leave their slot in the queue, dropped once they outnumber the pending timers.
TEST(WebClientTest, ClearedTimersAreDropped) {
  WebClient client;
  WebPage page = test::Navigate(client,
                                Page("<script>log = []; setTimeout(() => log.push('kept'), 10); const f = () => {};"
                                     "for (let i = 0; i < 1000; ++i) clearTimeout(setTimeout(f, 1));"
                                     "setTimeout(() => log.push('after'), 20)</script>"),
                                {.until = LoadState::NetworkIdle});
  EXPECT_EQ(Log(page), R"(["kept","after"])");
}

TEST(WebClientTest, SetTimeoutNeedsAHandler) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<p>x</p>"));
  EXPECT_FALSE(page.evaluate("setTimeout()"));
  EXPECT_FALSE(page.evaluate("setInterval()"));
  EXPECT_FALSE(page.evaluate("setTimeout(() => {}, Symbol())"));
  EXPECT_FALSE(page.evaluate("clearTimeout(Symbol())"));
  EXPECT_FALSE(page.evaluate("setTimeout({ toString() { throw new Error('handler') } })"));
}

TEST(WebClientTest, SelectorMatchedBeforeTheScripts) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<div id=data>served</div><script>window.ran = 1</script>"),
                                {.untilSelector = "#data"});
  EXPECT_EQ(page.stopReason(), StopReason::SelectorMatched);
  EXPECT_EQ(test::Eval(page, "typeof ran"), R"("undefined")");
}

TEST(WebClientTest, SelectorMatchedAfterAScript) {
  WebClient client;
  WebPage page = test::Navigate(
      client,
      Page("<script>window.first = 1</script><script>document.body.insertAdjacentHTML('beforeend', '<div "
           "class=late></div>')</script><script>window.after = 1</script>"),
      {.untilSelector = "div.late"});
  EXPECT_EQ(page.stopReason(), StopReason::SelectorMatched);
  EXPECT_EQ(test::Eval(page, "[window.first, window.after]"), "[1,null]");
}

TEST(WebClientTest, SelectorMatchedAfterATimer) {
  WebClient client;
  WebPage page = test::Navigate(
      client,
      Page("<script>setTimeout(() => document.body.append('x'), 500); setTimeout(() => "
           "document.body.appendChild(document.createElement('ul')), 1000); setTimeout(() => window.after = 1, 2000)"
           "</script>"),
      {.untilSelector = "ul", .until = LoadState::NetworkIdle});
  EXPECT_EQ(page.stopReason(), StopReason::SelectorMatched);
  EXPECT_EQ(test::Eval(page, "window.after"), "");
}

// A change that inserts nothing changes the document too: replaceChild() of an empty fragment removes the child.
TEST(WebClientTest, SelectorMatchedAfterAReplacementByNothing) {
  WebClient client;
  WebPage page = test::Navigate(client,
                                Page("<p id=gone></p><i></i><script>setTimeout(() => document.body.replaceChild("
                                     "document.createDocumentFragment(), document.getElementById('gone')), 10);"
                                     "setTimeout(() => window.after = 1, 20)</script>"),
                                {.untilSelector = "body > i:first-child", .until = LoadState::NetworkIdle});
  EXPECT_EQ(page.stopReason(), StopReason::SelectorMatched);
  EXPECT_EQ(test::Eval(page, "[document.getElementById('gone'), window.after]"), "[null,null]");
}

TEST(WebClientTest, SelectorNeverMatched) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<p>no match</p>"), {.untilSelector = "table"});
  EXPECT_EQ(page.stopReason(), StopReason::StateReached);
  EXPECT_THROW(
      { [[maybe_unused]] auto res = client.navigate(Page("<p>x</p>"), {.untilSelector = "["}); },
      std::invalid_argument);
}

TEST(WebClientTest, EndlessTimersAreStoppedByTheTimeout) {
  WebClientConfig config;
  config.maxIntervalRuns = std::numeric_limits<uint32_t>::max();
  config.maxTimerDelay = milliseconds::max();
  WebClient client(config);
  WebPage page = test::Navigate(client, Page("<script>setInterval(() => {}, 1000)</script>"),
                                {.until = LoadState::NetworkIdle, .timeout = milliseconds{100}});
  EXPECT_EQ(page.stopReason(), StopReason::TimedOut);
}

TEST(WebClientTest, ScriptNavigations) {
  const std::string target = PagePath("<p id=target>reached</p><script>window.loaded = location.pathname</script>");
  for (const std::string& assignment : {
           "location.href = '" + target + "'",
           "location.assign('" + target + "')",
           "location.replace('" + target + "')",
           "window.location = '" + target + "'",
           "document.location = '" + target + "'",
           "location.href = '/elsewhere'; location.href = '" + target + "'",
       }) {
    WebClient client;
    WebPage page = test::Navigate(client, Page("<script>" + assignment + "; window.stillRunning = 1</script>"));
    EXPECT_EQ(test::Eval(page, "[document.getElementById('target').textContent, loaded]"), R"(["reached","/html"])")
        << assignment;
    EXPECT_EQ(page.url(), StoreUrl(target));
  }
}

TEST(WebClientTest, LocationPartSetters) {
  WebClient client;
  // The script is in the query: its text must not contain the marker it looks for.
  WebPage search = test::Navigate(client, Page("<script>if (!location.search.endsWith('&done=1')) location.search = "
                                               "location.search + '&done=' + 1</script>"));
  EXPECT_TRUE(search.url().ends_with("&done=1")) << search.url();

  WebPage pathname = test::Navigate(client, Page("<script>location.pathname = 'echo-cookies'</script>"));
  EXPECT_TRUE(pathname.url().starts_with(StoreUrl("/echo-cookies?src="))) << pathname.url();
  WebPage absolutePath = test::Navigate(client, Page("<script>location.pathname = '/echo-cookies'</script>"));
  EXPECT_TRUE(absolutePath.document().querySelector("#cookies"));
  WebPage noQuery = test::Navigate(client, Page("<script>location.search = '?'</script>"));
  EXPECT_EQ(noQuery.url(), StoreUrl("/html"));
}

TEST(WebClientTest, FragmentNavigationsOnlyChangeTheUrl) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<script>location.hash = 'top'; window.hash = location.hash;"
                                             "location.href = '#other'; location.hash = '#third'</script>"));
  EXPECT_EQ(test::Eval(page, "hash"), R"("#top")");
  EXPECT_EQ(page.url(), Page("<script>location.hash = 'top'; window.hash = location.hash;"
                             "location.href = '#other'; location.hash = '#third'</script>") +
                            "#third");
}

TEST(WebClientTest, NavigationsIgnoredOrInvalid) {
  WebClient client;
  WebPage page =
      test::Navigate(client, Page("<script>location.href = 'javascript:void(0)'; location.assign('mailto:a@b.c');"
                                  "try { location.href = 'http://[' } catch (e) { window.error = e.name }</script>"));
  EXPECT_EQ(test::Eval(page, "error"), R"("SyntaxError")");
  EXPECT_TRUE(page.url().starts_with(StoreUrl("/html"))) << page.url();
  // A navigation started by evaluate() is ignored.
  EXPECT_EQ(test::Eval(page, "location.href = '/missing'; 1"), "1");
  EXPECT_EQ(page.status(), http::StatusCodeOK);
  EXPECT_TRUE(page.url().starts_with(StoreUrl("/html"))) << page.url();
}

TEST(WebClientTest, NavigationLimit) {
  WebClientConfig config;
  config.maxNavigations = 2;
  WebClient client(config);
  const std::string third = PagePath("<p id=third>");
  const std::string second = PagePath("<script>location.href = '" + third + "'</script>");
  EXPECT_TRUE(client.navigate(Page("<script>location.href = '" + second + "'</script>")));
  EXPECT_EQ(client.navigate(Page("<script>location.reload()</script>")).error(), WebErrc::tooManyNavigations);

  config.maxNavigations = 0;
  WebClient noNavigation(config);
  EXPECT_EQ(noNavigation.navigate(Page("<script>location.href = '/x'</script>")).error(), WebErrc::tooManyNavigations);
}

TEST(WebClientTest, NavigationInATimer) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script>setTimeout(() => location.href = '" + PagePath("<p id=t>") + "', 100)</script>"),
      {.until = LoadState::NetworkIdle});
  EXPECT_TRUE(page.document().querySelector("#t"));
}

TEST(WebClientTest, NavigationsWhileTheDocumentLoads) {
  WebClient client;
  const std::string target = PagePath("<p id=t>");
  // From a timer due at once, run between two scripts.
  WebPage fromTimer = test::Navigate(
      client, Page("<script>setTimeout(() => location.href = '" + target + "')</script><script>window.x = 1</script>"));
  EXPECT_TRUE(fromTimer.document().querySelector("#t"));
  // From a load listener.
  WebPage fromLoad =
      test::Navigate(client, Page("<script>window.onload = () => location.href = '" + target + "'</script>"));
  EXPECT_TRUE(fromLoad.document().querySelector("#t"));
  // A selector matched after an async script.
  WebPage async = test::Navigate(client,
                                 Page(Script("document.body.append(document.createElement('aside'))", "async") +
                                      "<script>window.addEventListener('load', () => window.loaded = true)</script>"),
                                 {.untilSelector = "aside"});
  EXPECT_EQ(async.stopReason(), StopReason::SelectorMatched);
  EXPECT_EQ(test::Eval(async, "window.loaded"), "");
}

TEST(WebClientTest, DeadlineInAnEventListener) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<script>window.addEventListener('load', () => { for (;;) {} })</script>"),
                                {.timeout = milliseconds{100}});
  EXPECT_EQ(page.stopReason(), StopReason::TimedOut);
  EXPECT_TRUE(page.uncaughtErrors().empty());
}

TEST(WebClientTest, MetaRefresh) {
  WebClient client;
  const std::string target = PagePath("<p id=refreshed>");
  for (const std::string& meta : {
           "content=\"0; url=" + target + "\"",
           "content=\"0;URL='" + target + "'\"",
           "content='0.5, url = \"" + target + "\"'",
           "content=\".9;" + target + "\"",
           // An invalid refresh is skipped: the next one is the refresh of the document.
           "content='soon'><meta http-equiv=refresh content='0; url=" + target + "'",
       }) {
    WebPage page = test::Navigate(client, Page("<meta http-equiv=Refresh " + meta + ">"));
    EXPECT_TRUE(page.document().querySelector("#refreshed")) << meta;
  }
  // Not followed: a refresh of the page itself, a non-http URL, or a delayed refresh before NetworkIdle. Only the
  // first valid refresh counts.
  for (const std::string_view content : {"0", "0; url=javascript:alert(1)", "3; url=/x"}) {
    WebPage page = test::Navigate(client, Page("<meta http-equiv=refresh content='" + std::string(content) +
                                               "'><meta http-equiv=refresh content='0; url=/missing'>"));
    EXPECT_EQ(page.status(), http::StatusCodeOK) << content;
    EXPECT_TRUE(page.url().starts_with(StoreUrl("/html"))) << content;
  }
}

TEST(WebClientTest, DelayedMetaRefreshAfterTheTimersDueBefore) {
  WebClientConfig config;
  config.maxTimerDelay = milliseconds{10000};
  WebClient client(config);

  NavigateOptions networkIdleOptions;
  networkIdleOptions.until = LoadState::NetworkIdle;

  WebPage page = test::Navigate(
      client,
      Page("<meta http-equiv=refresh content='2; url=/echo-cookies'><script>setTimeout(() => document.cookie = "
           "'before=1', 1000); setTimeout(() => document.cookie = 'after=1', 3000)</script>"),
      networkIdleOptions);
  EXPECT_EQ(page.document().querySelector("#cookies").textContent(), "before=1");
  // Beyond the horizon of the timers: not followed.
  WebPage beyondHorizon =
      test::Navigate(client, Page("<meta http-equiv=refresh content='11; url=/echo-cookies'>"), networkIdleOptions);
  EXPECT_TRUE(beyondHorizon.url().starts_with(StoreUrl("/html"))) << beyondHorizon.url();
}

TEST(WebClientTest, DocumentCookieHidesHttpOnlyCookies) {
  WebClient client;
  WebPage page = test::Navigate(client, StoreUrl("/cookies"));
  EXPECT_EQ(test::Eval(page, "[seen, after]"), R"(["visible=1","visible=1; mine=1"])");
  std::string jar;
  client.httpClient().cookieJar().forEach([&jar](const Cookie& cookie) {
    jar.append(cookie.name).append(1, '=').append(cookie.value).append(cookie.httpOnly ? "(httpOnly) " : " ");
  });
  EXPECT_EQ(jar, "secret=1(httpOnly) visible=1 mine=1 ");
  // The cookies of the scripts are sent with the next requests.
  WebPage echo = test::Navigate(client, StoreUrl("/echo-cookies"));
  EXPECT_EQ(echo.document().querySelector("#cookies").textContent(), "secret=1; visible=1; mine=1");
}

TEST(WebClientTest, QueueItLikeFlowAcrossSites) {
  WebClient client;
  WebPage page = test::Navigate(client, StoreUrl("/product"), {.untilSelector = "script#__NEXT_DATA__"});
  EXPECT_EQ(page.stopReason(), StopReason::SelectorMatched);
  EXPECT_EQ(page.url(), StoreUrl("/product?queueittoken=ok"));
  EXPECT_EQ(page.document().querySelector("script#__NEXT_DATA__").textContent(), R"({"stock":"OutOfStock"})");
  // The data was read before the scripts of the product page.
  EXPECT_EQ(test::Eval(page, "typeof productScriptRan"), R"("undefined")");
  EXPECT_EQ(client.httpClient().cookieJar().cookieHeader(QueueUrl("/")), "qtest=1");
  // The token cookie lives in the jar: the next visit goes straight to the product.
  WebPage again = test::Navigate(client, StoreUrl("/product"));
  EXPECT_EQ(again.url(), StoreUrl("/product"));
  EXPECT_EQ(test::Eval(again, "productScriptRan"), "true");
}

TEST(WebClientTest, StorageOfEachOrigin) {
  WebClient client;
  WebPage first = test::Navigate(client, Page("<p>"));
  EXPECT_EQ(test::Eval(first,
                       "localStorage.setItem('k', 'v'); localStorage.setItem('k', 'w'); "
                       "localStorage.setItem('n', 1); sessionStorage.setItem('s', 'session');"
                       "[localStorage.length, localStorage.key(0), localStorage.key(2), localStorage.key(-1), "
                       "localStorage.getItem('missing'), sessionStorage.getItem('s')]"),
            R"([2,"k",null,null,null,"session"])");
  // Shared by the pages of the origin, and kept across navigate() calls.
  WebPage second = test::Navigate(client, Page("<p>second</p>"));
  EXPECT_EQ(test::Eval(second, "[localStorage.getItem('k'), localStorage.getItem('n'), sessionStorage.length]"),
            R"(["w","1",1])");
  EXPECT_EQ(test::Eval(second, "localStorage.removeItem('k'); localStorage.removeItem('none'); localStorage.key(0)"),
            R"("n")");
  // Another origin has its own.
  WebPage other = test::Navigate(client, test::PageUrl(gServer, "<p>", "localhost"));
  EXPECT_EQ(test::Eval(other, "[localStorage.length, sessionStorage.length]"), "[0,0]");
  EXPECT_EQ(test::Eval(second, "localStorage.clear(); localStorage.length"), "0");
  EXPECT_EQ(test::Eval(second, "localStorage.getItem()"), "null");
  EXPECT_FALSE(second.evaluate("localStorage.key(Symbol())"));
  EXPECT_FALSE(second.evaluate("Object.getPrototypeOf(localStorage).getItem.call({}, 'k')"));
}

// The bindings of the storage need a storage object, and propagate the exceptions of their conversions.
TEST(WebClientTest, StorageErrors) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<p>"));
  EXPECT_EQ(test::Eval(page, R"(
    const boom = { toString() { throw new Error('boom') } };
    const storage = Object.getPrototypeOf(localStorage);
    const length = Object.getOwnPropertyDescriptor(storage, 'length').get;
    [
      () => localStorage.getItem(boom), () => localStorage.setItem(boom, 'v'), () => localStorage.setItem('k', boom),
      () => localStorage.removeItem(boom), () => localStorage.key({ valueOf() { throw new Error('boom') } }),
    ].map(call => { try { call(); return 'no error'; } catch (e) { return e.message; } }).concat(
      [storage.getItem, storage.setItem, storage.removeItem, storage.clear, storage.key, length].map(f => {
        try { f.call({}, 'k', 'v'); return 'no error'; } catch (e) { return e.name; }
      })))"),
            R"(["boom","boom","boom","boom","boom","TypeError","TypeError","TypeError","TypeError","TypeError",)"
            R"("TypeError"])");
  EXPECT_EQ(test::Eval(page, "localStorage.length"), "0");
}

// The location setters propagate the exceptions of their conversions, without navigation.
TEST(WebClientTest, LocationConversionErrors) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<p>"));
  const std::string url(page.url());
  EXPECT_EQ(test::Eval(page, R"(
    const boom = { toString() { throw new Error('boom') } };
    [
      () => location.href = boom, () => window.location = boom, () => location.hash = boom,
      () => location.search = boom, () => location.pathname = boom, () => location.assign(boom),
      () => location.replace(boom), () => history.pushState(null, '', boom),
    ].map(call => { try { call(); return 'no error'; } catch (e) { return e.message; } }))"),
            R"(["boom","boom","boom","boom","boom","boom","boom","boom"])");
  EXPECT_EQ(page.url(), url);
}

TEST(WebClientTest, StorageQuota) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<p>"));
  EXPECT_EQ(test::Eval(page,
                       "const big = 'x'.repeat(3 << 20); localStorage.setItem('a', big);"
                       "try { localStorage.setItem('b', big) } catch (e) { window.error = e.name }"
                       "localStorage.setItem('a', 'small'); localStorage.setItem('b', big);"
                       "[error, localStorage.length]"),
            R"(["QuotaExceededError",2])");
}

TEST(WebClientTest, History) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<p>"));
  EXPECT_EQ(test::Eval(page,
                       "history.pushState({n: 1}, '', '/new?x=1#f'); [history.length, history.state.n, "
                       "location.pathname, history.scrollRestoration]"),
            R"([2,1,"/new","auto"])");
  EXPECT_EQ(page.url(), StoreUrl("/new?x=1#f"));
  EXPECT_EQ(test::Eval(page,
                       "history.replaceState(null, ''); history.scrollRestoration = 'manual'; history.back();"
                       "history.forward(); history.go(-1); [history.length, history.state]"),
            "[2,null]");
  EXPECT_EQ(page.url(), StoreUrl("/new?x=1#f"));
  // Another origin, or user information (HTML "can have its URL rewritten").
  for (const std::string_view url : {"'http://localhost/'", "location.origin.replace('//', '//user@') + '/'"}) {
    const auto crossOrigin = page.evaluate("history.pushState(null, '', " + std::string(url) + ")");
    ASSERT_FALSE(crossOrigin) << url;
    EXPECT_TRUE(crossOrigin.error().contains("SecurityError")) << crossOrigin.error();
  }
  EXPECT_EQ(page.url(), StoreUrl("/new?x=1#f"));
  EXPECT_FALSE(page.evaluate("history.replaceState(null, '', 'http://[')"));
}

TEST(WebClientTest, LocationAndNavigator) {
  WebClient client;
  const std::string url = Page("<p>") + "#frag";
  WebPage page = test::Navigate(client, url);
  const std::string host = "127.0.0.1:" + std::to_string(gServer.port());
  EXPECT_EQ(test::Eval(page,
                       "[location.protocol, location.host, location.hostname, location.port, location.pathname, "
                       "location.search.startsWith('?src='), location.hash, location.origin, String(location), "
                       "location.toString() === location.href, window.origin === location.origin]"),
            R"(["http:",")" + host + R"(","127.0.0.1",")" + std::to_string(gServer.port()) +
                R"(","/html",true,"#frag","http://)" + host + R"(",")" + url + R"(",true,true])");
  EXPECT_EQ(test::Eval(page,
                       "[navigator.userAgent, navigator.appVersion, navigator.language, navigator.languages, "
                       "navigator.cookieEnabled, navigator.onLine, typeof navigator.webdriver]"),
            R"(["aeronet","aeronet","en-US",["en-US"],true,true,"undefined"])");

  WebClientConfig config;
  const http::Header headers[]{{"User-Agent", "Mozilla/5.0 (Test)"}, {"Accept-Language", "fr-FR,fr;q=0.9"}};
  config.http.withGlobalHeaders(headers);
  WebClient custom(config);
  WebPage customPage = test::Navigate(custom, Page("<p>"));
  EXPECT_EQ(test::Eval(customPage, "[navigator.userAgent, navigator.appVersion, navigator.language]"),
            R"json(["Mozilla/5.0 (Test)","5.0 (Test)","fr-FR"])json");
}

TEST(WebClientTest, BaseUrl) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<base href='/sub/'><script src='base.js'></script>"
                                             "<script>location.href = '../echo-cookies'</script>"));
  EXPECT_TRUE(page.url().starts_with(StoreUrl("/echo-cookies"))) << page.url();
  WebPage withBase = test::Navigate(client, Page("<base href='/sub/'><script src='base.js'></script>"));
  EXPECT_EQ(test::Eval(withBase, "fromBase"), "true");
}

TEST(WebClientTest, RedirectsGiveTheFinalUrl) {
  WebClient client;
  const std::string target = PagePath("<p id=t>");
  WebPage page = test::Navigate(client, StoreUrl("/redirect?to=" + test::PercentEncode(target)) + "#kept");
  EXPECT_EQ(page.url(), StoreUrl(target) + "#kept");
  // The fragment of a Location replaces the one of the requested URL (RFC 9110 §10.2.2), that of the last redirect
  // with one.
  WebPage own = test::Navigate(client, StoreUrl("/redirect?to=" + test::PercentEncode(target + "#own")) + "#asked");
  EXPECT_EQ(own.url(), StoreUrl(target) + "#own");
  const std::string second = "/redirect?to=" + test::PercentEncode(target);
  WebPage chain = test::Navigate(client, StoreUrl("/redirect?to=" + test::PercentEncode(second + "#first")) + "#asked");
  EXPECT_EQ(chain.url(), StoreUrl(target) + "#first");
  // A Location without path is requested with the path "/" (no page there).
  WebPage root = test::Navigate(client, StoreUrl("/redirect?to=" + test::PercentEncode(StoreUrl("?x=1"))));
  EXPECT_EQ(root.url(), StoreUrl("/?x=1"));
  EXPECT_EQ(root.status(), http::StatusCodeNotFound);
}

// The cookie jar of the HTTP client, which the pages need, is enabled with its default limit unless the configuration
// enables it.
TEST(WebClientTest, CookieJarOfTheConfiguration) {
  WebClientConfig config;
  config.http.withCookies(5);
  EXPECT_EQ(WebClient(config).httpClient().config().cookies.maxCookies, 5U);
  EXPECT_EQ(WebClient().httpClient().config().cookies.maxCookies, HttpClientConfig::Cookies::kDefaultMaxCookies);
}

TEST(WebClientTest, ThroughAForwardProxy) {
  WebClientConfig config;
  config.http.withProxy(test::ServerUrl(gServer));
  WebClient client(config);
  // The proxy (the test server) serves the request of the absolute-form target itself.
  WebPage page = test::Navigate(client, "http://example.invalid" + PagePath("<p id=proxied>"));
  EXPECT_TRUE(page.document().querySelector("#proxied"));
  EXPECT_EQ(page.url(), "http://example.invalid" + PagePath("<p id=proxied>"));
}

TEST(WebClientTest, ErrorPagesAreLoaded) {
  WebClient client;
  WebPage page = test::Navigate(client, StoreUrl("/missing"));
  EXPECT_EQ(page.status(), http::StatusCodeNotFound);
  EXPECT_EQ(test::Eval(page, "ran"), "true");
}

TEST(WebClientTest, Failures) {
  WebClient client;
  for (const std::string_view url : {"ftp://example.com/", "not a url", "", "javascript:alert(1)"}) {
    EXPECT_EQ(client.navigate(url).error(), WebErrc::invalidUrl) << url;
  }
  EXPECT_EQ(client.navigate(ClosedPortUrl()).error(), WebErrc::requestFailed);
  // Valid http URLs that the HTTP client cannot request: user info, port 0, from navigate() or from a page.
  const std::string port = std::to_string(gServer.port());
  EXPECT_EQ(client.navigate("http://user@127.0.0.1:" + port + "/html").error(), WebErrc::invalidUrl);
  // The URL parser keeps an incomplete percent-escape, that the HTTP client refuses.
  EXPECT_EQ(client.navigate("http://127.0.0.1:" + port + "/%zz").error(), WebErrc::invalidUrl);
  EXPECT_EQ(client.navigate(Page("<script>location.href = 'http://user@127.0.0.1:" + port + "/'</script>")).error(),
            WebErrc::invalidUrl);
  EXPECT_EQ(client.navigate(Page("<meta http-equiv=refresh content='0; url=http://127.0.0.1:0/'>")).error(),
            WebErrc::invalidUrl);
  EXPECT_EQ(client.navigate(Page("<script>location.href = '" + ClosedPortUrl() + "'</script>")).error(),
            WebErrc::requestFailed);
}

TEST(WebUrlTest, SplitUrl) {
  constexpr internal::UrlParts kParts = internal::SplitUrl("https://user:pw@[::1]:8443/a/b?q=1#h");
  static_assert(kParts.protocol == "https:" && kParts.host == "[::1]:8443" && kParts.hostname == "[::1]" &&
                kParts.port == "8443" && kParts.pathname == "/a/b" && kParts.search == "?q=1" && kParts.hash == "#h");
  constexpr internal::UrlParts kIpv6 = internal::SplitUrl("http://[::1]/?#");
  static_assert(kIpv6.hostname == "[::1]" && kIpv6.port.empty() && kIpv6.search.empty() && kIpv6.hash.empty());
  constexpr internal::UrlParts kNoAuthority = internal::SplitUrl("about:blank");
  static_assert(kNoAuthority.protocol == "about:" && kNoAuthority.host.empty() && kNoAuthority.pathname == "blank");
  constexpr internal::UrlParts kHostOnly = internal::SplitUrl("http://host");
  static_assert(kHostOnly.host == "host" && kHostOnly.pathname.empty());
  EXPECT_EQ(internal::WithoutFragment("http://h/p#f"), "http://h/p");
  // At runtime too.
  const std::string about = "about:blank";
  const internal::UrlParts aboutParts = internal::SplitUrl(about);
  EXPECT_TRUE(aboutParts.host.empty());
  EXPECT_EQ(aboutParts.pathname, "blank");
  const std::string ipv6 = "http://[::1]";
  const internal::UrlParts ipv6Parts = internal::SplitUrl(ipv6);
  EXPECT_EQ(ipv6Parts.hostname, "[::1]");
  EXPECT_TRUE(ipv6Parts.port.empty());
  EXPECT_TRUE(ipv6Parts.pathname.empty());
  const std::string https = "https://h/";
  EXPECT_TRUE(internal::IsHttpUrl(https));
  EXPECT_FALSE(internal::IsHttpUrl(about));
  const std::string url = "http://user@host:81/p?#";
  const internal::UrlParts parts = internal::SplitUrl(url);
  EXPECT_EQ(parts.host, "host:81");
  EXPECT_EQ(parts.port, "81");
  EXPECT_EQ(parts.pathname, "/p");
  EXPECT_TRUE(parts.search.empty());
  EXPECT_TRUE(parts.hash.empty());
}

}  // namespace
}  // namespace aeronet
