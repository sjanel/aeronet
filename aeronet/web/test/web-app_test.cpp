// Single-page applications against local servers: the scripts that scripts insert (webpack chunk loading), a
// production build of React rendering a table, the observers, and the Intl API of the prelude.
#include <gtest/gtest.h>

#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "../src/js-module.hpp"
#include "aeronet/chrono-lite.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/web-client-config.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"
#include "web-test-util.hpp"

namespace aeronet {
namespace {

// The contents of the file 'name' of the test data.
std::string ReadTestData(std::string_view name) {
  std::ifstream file(std::string(AERONET_WEB_TEST_DATA_DIR) + '/' + std::string(name), std::ios::binary);
  EXPECT_TRUE(file.is_open()) << name;
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// The rows of the table of the React page.
std::string RowsJson() {
  std::string json = "[";
  for (int row = 0; row < 50; ++row) {
    json += (row == 0 ? "" : ",") + std::string(R"({"asset":"A)") + std::to_string(row) + R"(","fee":")" +
            std::to_string(row) + ".5\"}";
  }
  return json + "]";
}

test::TestServer MakeServer() {
  test::TestServer server = test::MakePageServer();
  auto router = server.router();
  router.setPath(http::Method::GET, "/missing",
                 [](const HttpRequestView& req) { return req.makeResponse(http::StatusCodeNotFound); });
  // Copied by value: the handlers run on the thread of the server.
  for (const std::string_view file : {"react.production.min.js", "react-dom.production.min.js"}) {
    router.setPath(http::Method::GET, "/react/" + std::string(file),
                   [code = ReadTestData("react-18.3.1/" + std::string(file))](const HttpRequestView& req) {
                     return req.makeResponse(code, "text/javascript");
                   });
  }
  router.setPath(http::Method::GET, "/rows", [rows = RowsJson()](const HttpRequestView& req) {
    return req.makeResponse(rows, http::ContentTypeApplicationJson);
  });
  return server;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gServer = MakeServer();

std::string Page(std::string_view html) { return test::PageUrl(gServer, html); }

using test::Log;
using test::ScriptPath;

// A script, inside a page script: creates a script element whose attributes are those of 'init' (an object literal of
// JavaScript), then inserts it at the end of the head.
std::string InsertScript(std::string_view init) {
  return "document.head.appendChild(Object.assign(document.createElement('script'), " + std::string(init) + "));";
}

TEST(WebAppTest, InsertedInlineScriptsRunAtOnce) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script id=outer>log = []; const s = document.createElement('script'); s.id = 'inner';"
                   "s.textContent = \"log.push('inserted ' + document.currentScript.id)\"; log.push('before');"
                   "document.body.appendChild(s); log.push('after ' + document.currentScript.id);"
                   // An error of the inserted script is reported, not thrown to the insertion.
                   "const failing = document.createElement('script'); failing.text = 'throw new Error(\"inner\")';"
                   "document.body.append(failing); log.push('not thrown');"
                   // Run once: neither a move nor a new text runs it again.
                   "document.head.appendChild(s); s.textContent = \"log.push('again')\";</script>"));
  EXPECT_EQ(Log(page), R"(["before","inserted inner","after outer","not thrown"])");
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].starts_with("Error: inner")) << page.uncaughtErrors()[0];
  EXPECT_EQ(test::Eval(page, "document.currentScript"), "null");
}

TEST(WebAppTest, InsertedExternalScriptsRunAsTasks) {
  const std::string html =
      "<script>log = []; window.addEventListener('load', () => log.push('load'));"
      "document.addEventListener('DOMContentLoaded', () => log.push('DOMContentLoaded'));"
      "for (const name of ['one', 'two']) {" +
      InsertScript("{src: '" + ScriptPath("log.push(document.currentScript.dataset.n + ' ran')") +
                   "', onload: e => log.push(name + ' ' + e.type)}") +
      "document.head.lastChild.dataset.n = name; }" +
      InsertScript("{src: '/missing', onerror: e => log.push('missing ' + e.type)}") +
      InsertScript("{src: 'http://[bad', onerror: e => log.push('invalid ' + e.type)}") +
      "log.push('end of the script')</script><script>log.push('next parser script')</script>";
  WebClient client;
  WebPage page = test::Navigate(client, Page(html));
  // Fetched and run after the task that inserted them, before the next one: they delay the events of the document.
  EXPECT_EQ(Log(page), R"(["end of the script","one ran","one load","two ran","two load","missing error",)"
                       R"("invalid error","next parser script","DOMContentLoaded","load"])");
  EXPECT_TRUE(page.uncaughtErrors().empty());
}

// An inserted external script runs what its src named when it was inserted: a later change of its attributes does not
// change what runs (HTML "prepare the script element" reads them).
TEST(WebAppTest, InsertedExternalScriptsKeepTheirSource) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script>log = []; const first = Object.assign(document.createElement('script'), {src: '" +
                   ScriptPath("log.push('first')") +
                   "', text: \"log.push('text')\"}); document.head.append(first); first.removeAttribute('src');"
                   "const second = Object.assign(document.createElement('script'), {src: '" +
                   ScriptPath("log.push('second')") + "'}); document.head.append(second); second.src = '" +
                   ScriptPath("log.push('changed')") +
                   "'; const empty = Object.assign(document.createElement('script'), {src: '', onerror: () => "
                   "log.push('empty src')}); document.head.append(empty); empty.src = '" +
                   ScriptPath("log.push('src set after')") + "'; log.push('end')</script>"));
  EXPECT_EQ(Log(page), R"(["end","first","second","empty src"])");
}

TEST(WebAppTest, ScriptsOfHtmlFragmentsNeverRun) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script>log = []; document.body.innerHTML = \"<script>log.push('innerHTML')<\\/script>\";"
                   "document.body.insertAdjacentHTML('beforeend', \"<script>log.push('adjacent')<\\/script>\");"
                   "const div = document.createElement('div'); div.innerHTML = \"<script>log.push('div')<\\/script>\";"
                   "document.body.append(div, document.body.firstChild);"
                   // A parser script does not run again either.
                   "document.body.append(document.currentScript); log.push('done')</script>"));
  EXPECT_EQ(Log(page), R"(["done"])");
}

TEST(WebAppTest, EmptyInsertedScriptsWaitForTheirTextOrSrc) {
  WebClient client;
  WebPage page = test::Navigate(
      client,
      Page("<script>log = []; const make = () => document.body.appendChild(document.createElement('script'));"
           "const byText = make(); log.push('empty'); byText.textContent = \"log.push('text')\";"
           "byText.textContent = \"log.push('text again')\";"
           "make().appendChild(document.createTextNode(\"log.push('child')\"));"
           "make().innerHTML = \"log.push('innerHTML')\";"
           "make().setAttribute('src', '" +
           ScriptPath("log.push('src')") +
           "');"
           // A script out of the document does not run, an empty text does not start it.
           "const detached = document.createElement('script'); detached.text = \"log.push('detached')\";"
           "const blank = make(); blank.textContent = ''; blank.text = \"log.push('blank')\"; "
           // Nor a comment, nor an empty text node.
           "const other = make(); other.append(document.createComment('c'), document.createTextNode(''));"
           "other.append(\"log.push('other')\");"
           // Any change of its children: an adjacent text or HTML, the data of its text.
           "make().insertAdjacentText('afterbegin', \"log.push('adjacent text')\");"
           "make().insertAdjacentHTML('beforeend', \"log.push('adjacent HTML')\");"
           "make().appendChild(document.createElement('i')).insertAdjacentText('afterend', \"log.push('after')\");"
           "const data = make().appendChild(document.createTextNode('')); data.data = \"log.push('data')\";"
           "data.data = \"log.push('data again')\"; log.push('end')</script>"));
  EXPECT_EQ(Log(page), R"(["empty","text","child","innerHTML","blank","other","adjacent text","adjacent HTML",)"
                       R"("after","data","end","src"])");
}

TEST(WebAppTest, TypesOfInsertedScripts) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script>log = []; for (const type of ['application/json', 'module', 'text/javascript', '']) {"
                   "const s = document.createElement('script'); s.type = type;"
                   "s.text = `log.push('${type}')`; document.body.append(s); }"
                   "const json = document.body.lastChild.previousSibling.previousSibling.previousSibling;"
                   // A data block becomes a classic script when its type changes, and its text is inserted again.
                   "json.type = 'text/javascript'; json.append(';');</script>"));
  // The module runs as a task, after the script that inserted it (not at all without module support).
  EXPECT_EQ(Log(page), internal::ModulesEnabled() ? R"(["text/javascript","","application/json","module"])"
                                                  : R"(["text/javascript","","application/json"])");
}

TEST(WebAppTest, ClonedScripts) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script id=parser>log = []; const created = document.createElement('script');"
                   "created.text = \"log.push('created')\"; document.body.append(created.cloneNode(true), created);"
                   "document.body.append(created.cloneNode(true), document.currentScript.cloneNode(true));"
                   // The scripts of a deep copy too, in a template copied with its contents as well.
                   "const div = document.createElement('div'); const nested = document.createElement('script');"
                   "nested.text = \"log.push('nested')\"; div.append(nested); document.body.append(div.cloneNode(),"
                   "div.cloneNode(true)); const template = document.createElement('template');"
                   "template.content.append(nested); document.body.append(template.cloneNode(true).content);"
                   "log.push('end')</script>"));
  // The copies of a script not started run, not those of a started one (parser scripts started).
  EXPECT_EQ(Log(page), R"(["created","created","nested","nested","end"])");
}

TEST(WebAppTest, ScriptsOfAnInsertedTreeRunInTreeOrder) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script>log = []; const fragment = document.createDocumentFragment();"
                   "const script = text => Object.assign(document.createElement('script'), {text});"
                   "const div = document.createElement('div'); div.append(script(\"log.push('nested')\"));"
                   "fragment.append(script(\"log.push('first'); document.getElementById('third').remove()\"), div,"
                   "Object.assign(script(\"log.push('third')\"), {id: 'third'}), script(\"log.push('last')\"));"
                   "log.push('insert'); document.body.append(fragment); log.push('inserted')</script>"));
  // An earlier script of the insertion removed the third one: it is not connected anymore when its turn comes.
  EXPECT_EQ(Log(page), R"(["insert","first","nested","last","inserted"])");
}

TEST(WebAppTest, ScriptStartedByAnEarlierScriptOfTheSameInsertion) {
  WebClient client;
  WebPage page = test::Navigate(
      client,
      Page("<script>log = []; const fragment = document.createDocumentFragment();"
           "const script = text => Object.assign(document.createElement('script'), {text});"
           "const later = script(\"log.push('later')\");"
           // The first script moves the later one, which runs then: not a second time when its turn comes.
           "window.later = later; fragment.append(script(\"log.push('first'); document.head.append(window.later);"
           "log.push('moved')\"), later); document.body.append(fragment)</script>"));
  EXPECT_EQ(Log(page), R"(["first","later","moved"])");
}

TEST(WebAppTest, DocumentWriteOfInsertedScripts) {
  WebClient client;
  WebPage page = test::Navigate(
      client,
      Page("<body><script id=parser>log = [];" + InsertScript("{text: \"document.write('<i id=inline>')\"}") +
           InsertScript("{src: '" + ScriptPath("document.write('<i id=external>'); log.push('external')") + "'}") +
           "</script><p id=after></p></body>"));
  // An inline script inserted by a parser script writes after it; an external one cannot write.
  EXPECT_EQ(Log(page), R"(["external"])");
  EXPECT_EQ(test::Eval(page,
                       "[document.getElementById('inline').previousElementSibling.id,"
                       "document.getElementById('inline').nextElementSibling.id,"
                       "document.getElementById('external')]"),
            R"(["parser","after",null])");
}

TEST(WebAppTest, InsertedScriptsCountTowardMaxRequests) {
  WebClientConfig config;
  config.maxRequests = 2;
  WebClient client(std::move(config));
  WebPage page = test::Navigate(
      client, Page("<script>log = []; for (let i = 0; i < 3; ++i) {" +
                   InsertScript("{src: '" + ScriptPath("log.push('ran')") +
                                "', onerror: () => log.push('refused ' + i), onload: () => log.push('loaded ' + i)}") +
                   "}</script>"));
  EXPECT_EQ(Log(page), R"(["ran","loaded 0","ran","loaded 1","refused 2"])");
}

// The inline scripts inserted by scripts count toward WebClientConfig::maxScriptBytes too: one beyond it does not run,
// an error that does not reach the script that inserted it.
TEST(WebAppTest, InsertedScriptsCountTowardMaxScriptBytes) {
  const std::string parserScript =
      "log = []; document.body.append(Object.assign(document.createElement('script'), {text: 'log.push(1)'.padEnd(100)"
      "})); log.push('after')";
  WebClientConfig config;
  config.maxScriptBytes = parserScript.size() + 50;
  WebClient client(std::move(config));
  WebPage page = test::Navigate(client, Page("<script>" + parserScript + "</script>"));
  EXPECT_EQ(Log(page), R"(["after"])");
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].starts_with("aeronet: script inline not run")) << page.uncaughtErrors()[0];
}

TEST(WebAppTest, ScriptsInsertedAfterLoadRunWithNetworkIdle) {
  const std::string html = "<script>log = []; setTimeout(() => {" +
                           InsertScript("{src: '" + ScriptPath("log.push('late')") + "'}") + "}, 10)</script>";
  WebClient client;
  WebPage loaded = test::Navigate(client, Page(html));
  EXPECT_EQ(Log(loaded), "[]");
  WebPage idle = test::Navigate(client, Page(html), {.until = LoadState::NetworkIdle});
  EXPECT_EQ(Log(idle), R"(["late"])");
  // evaluate() runs the inline scripts it inserts, not the external ones: they are tasks.
  EXPECT_EQ(test::Eval(idle, InsertScript("{text: \"log.push('evaluated')\"}") +
                                 InsertScript("{src: '" + ScriptPath("log.push('never')") + "'}") + "log"),
            R"(["late","evaluated"])");
}

// The script loading of the runtime of webpack 5 (__webpack_require__.l and the JSONP chunk callback), written for
// this test: a chunk is a script pushing its modules to a global array.
constexpr std::string_view kWebpackRuntime = R"js(
(() => {
  const modules = {};
  const installed = {};  // chunk id -> 0 when loaded, [resolve, reject, promise] while loading
  const inProgress = {};
  const loadScript = (url, done, key) => {
    if (inProgress[url]) { inProgress[url].push(done); return; }
    let script;
    for (const candidate of document.getElementsByTagName('script')) {
      if (candidate.getAttribute('src') === url || candidate.getAttribute('data-webpack') === 'app:' + key) {
        script = candidate;
        break;
      }
    }
    let needAttach = false;
    if (!script) {
      needAttach = true;
      script = document.createElement('script');
      script.charset = 'utf-8';
      script.timeout = 120;
      script.setAttribute('data-webpack', 'app:' + key);
      script.src = url;
    }
    inProgress[url] = [done];
    const onScriptComplete = (prev, event) => {
      script.onerror = script.onload = null;
      clearTimeout(timeout);
      const doneFns = inProgress[url];
      delete inProgress[url];
      script.parentNode && script.parentNode.removeChild(script);
      doneFns && doneFns.forEach(fn => fn(event));
      if (prev) return prev(event);
    };
    const timeout = setTimeout(onScriptComplete.bind(null, undefined, { type: 'timeout', target: script }), 120000);
    script.onerror = onScriptComplete.bind(null, script.onerror);
    script.onload = onScriptComplete.bind(null, script.onload);
    needAttach && document.head.appendChild(script);
  };
  const loadChunk = (chunkId, url) => {
    if (installed[chunkId] === 0) return Promise.resolve();
    if (installed[chunkId]) return installed[chunkId][2];
    const promise = new Promise((resolve, reject) => { installed[chunkId] = [resolve, reject]; });
    installed[chunkId][2] = promise;
    loadScript(url, event => {
      if (installed[chunkId] !== 0) {
        const error = new Error('Loading chunk ' + chunkId + ' failed. (' + (event && event.type) + ')');
        installed[chunkId][1](error);
        installed[chunkId] = undefined;
      }
    }, 'chunk-' + chunkId);
    return promise;
  };
  const jsonpCallback = (parentPush, [chunkIds, moreModules]) => {
    Object.assign(modules, moreModules);
    if (parentPush) parentPush([chunkIds, moreModules]);
    for (const id of chunkIds) {
      if (installed[id]) installed[id][0]();
      installed[id] = 0;
    }
  };
  const chunkLoadingGlobal = self.webpackChunkapp = self.webpackChunkapp || [];
  chunkLoadingGlobal.forEach(jsonpCallback.bind(null, 0));
  chunkLoadingGlobal.push = jsonpCallback.bind(null, chunkLoadingGlobal.push.bind(chunkLoadingGlobal));
  window.__require = id => modules[id]();
  window.__load = loadChunk;
})();
)js";

TEST(WebAppTest, WebpackChunkLoading) {
  const std::string table =
      "(self.webpackChunkapp = self.webpackChunkapp || []).push([['table'], {"
      "render: () => { const rows = window.__require('rows'); document.body.insertAdjacentHTML('beforeend',"
      "'<table>' + rows.map(r => '<tr><td>' + r + '</td></tr>').join('') + '</table>'); } }])";
  const std::string rows =
      "(self.webpackChunkapp = self.webpackChunkapp || []).push([['rows'], {"
      "rows: () => ['BTC', 'ETH', 'SOL'] }])";
  const std::string html =
      "<script>" + std::string(kWebpackRuntime) + "</script><script>log = [];" +
      "__load('missing', '/missing').catch(error => log.push(error.message));"
      "Promise.all([__load('rows', '" +
      ScriptPath(rows) + "'), __load('table', '" + ScriptPath(table) +
      "')]).then(() => { __require('render'); log.push('rendered ' + document.querySelectorAll('script').length); });"
      "</script>";
  WebClient client;
  WebPage page = test::Navigate(client, Page(html), {.untilSelector = "table tr"});
  EXPECT_EQ(page.stopReason(), StopReason::SelectorMatched);
  // The loaded chunks removed their scripts.
  EXPECT_EQ(Log(page), R"json(["Loading chunk missing failed. (error)","rendered 2"])json");
  EXPECT_EQ(test::Eval(page, "[...document.querySelectorAll('td')].map(td => td.textContent)"),
            R"(["BTC","ETH","SOL"])");
  EXPECT_TRUE(page.uncaughtErrors().empty()) << page.uncaughtErrors()[0];
}

constexpr std::string_view kReactApp = R"js(
const e = React.createElement;
function Table() {
  const [rows, setRows] = React.useState([]);
  const [clicks, setClicks] = React.useState(0);
  React.useEffect(() => { fetch('/rows').then(response => response.json()).then(setRows); }, []);
  return e('div', null,
    e('button', { onClick: () => setClicks(count => count + 1) }, 'clicked ' + clicks),
    e('svg', { width: 10 }, e('circle', { r: 5 })),
    e('table', null, e('tbody', null, rows.map(row =>
      e('tr', { key: row.asset }, e('td', { className: 'asset' }, row.asset), e('td', null, row.fee))))));
}
ReactDOM.createRoot(document.getElementById('root')).render(e(Table));
)js";

TEST(WebAppTest, ReactRendersATableFromAnApi) {
  const std::string html =
      "<div id=root></div><script src=/react/react.production.min.js></script>"
      "<script src=/react/react-dom.production.min.js></script><script>" +
      std::string(kReactApp) + "</script>";
  WebClient client;
  WebPage page = test::Navigate(
      client, Page(html),
      {.untilSelector = "table tr", .until = LoadState::NetworkIdle, .timeout = std::chrono::seconds{20}});
  ASSERT_EQ(page.stopReason(), StopReason::SelectorMatched)
      << (page.uncaughtErrors().empty() ? "" : page.uncaughtErrors()[0]);
  EXPECT_TRUE(page.uncaughtErrors().empty()) << page.uncaughtErrors()[0];
  EXPECT_EQ(test::Eval(page,
                       "[document.querySelectorAll('tr').length, document.querySelector('td.asset').textContent,"
                       "document.querySelector('tr:last-child').textContent,"
                       "document.querySelector('circle').namespaceURI]"),
            R"([50,"A0","A4949.5","http://www.w3.org/2000/svg"])");
  // A click through the event delegation of React updates the state, rendered by its microtask.
  EXPECT_EQ(test::Eval(page,
                       "document.querySelector('button').dispatchEvent(new Event('click', {bubbles: true}));"
                       "document.querySelector('button').textContent"),
            R"("clicked 0")");
  EXPECT_EQ(test::Eval(page, "document.querySelector('button').textContent"), R"("clicked 1")");
}

TEST(WebAppTest, ReactSynchronousRenders) {
  const std::string html =
      "<div id=legacy></div><div id=root></div><script src=/react/react.production.min.js></script>"
      "<script src=/react/react-dom.production.min.js></script><script>"
      "const e = React.createElement;"
      "ReactDOM.render(e('ul', null, ['a', 'b'].map(x => e('li', {key: x}, x))), document.getElementById('legacy'));"
      "const root = ReactDOM.createRoot(document.getElementById('root'));"
      "ReactDOM.flushSync(() => root.render(e('p', {id: 'sync', 'data-x': 1}, 'flushed')));"
      "log = [document.querySelectorAll('li').length, document.getElementById('sync').textContent];</script>";
  WebClient client;
  WebPage page = test::Navigate(client, Page(html));
  EXPECT_EQ(Log(page), R"([2,"flushed"])");
  EXPECT_EQ(test::Eval(page, "document.getElementById('root').innerHTML"),
            R"("<p id=\"sync\" data-x=\"1\">flushed</p>")");
  EXPECT_TRUE(page.uncaughtErrors().empty()) << page.uncaughtErrors()[0];
}

TEST(WebAppTest, IntersectionObserverReportsEverythingVisible) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<p id=a></p><p id=b></p><p id=c></p><script>log = [];"
                   "const observer = new IntersectionObserver(function (entries, self) {"
                   "  log.push(this === observer && self === observer, entries.map(entry => [entry.target.id,"
                   "  entry.isIntersecting, entry.intersectionRatio, entry instanceof IntersectionObserverEntry,"
                   "  entry.boundingClientRect.width, entry.rootBounds.width]));"
                   "}, {threshold: [0.5, 0]});"
                   "for (const id of ['a', 'b', 'c', 'a']) observer.observe(document.getElementById(id));"
                   "observer.unobserve(document.getElementById('b'));"
                   "log.push(observer.thresholds, observer.root, observer.rootMargin);"
                   "const silent = new IntersectionObserver(() => log.push('silent'));"
                   "silent.observe(document.body); silent.disconnect();"
                   "const taken = new IntersectionObserver(() => log.push('taken'));"
                   "taken.observe(document.body); log.push(taken.takeRecords().length);</script>"));
  EXPECT_EQ(Log(page),
            R"([[0,0.5],null,"0px 0px 0px 0px",1,true,[["a",true,1,true,0,1280],["c",true,1,true,0,1280]]])");
  EXPECT_EQ(test::Eval(page, "'intersectionRatio' in IntersectionObserverEntry.prototype"), "true");
  for (const std::string_view script : {
           "new IntersectionObserver()",
           "new IntersectionObserver(() => {}, {threshold: 2})",
           "new IntersectionObserver(() => {}).observe({})",
           "IntersectionObserver(() => {})",
       }) {
    auto result = page.evaluate(script);
    ASSERT_FALSE(result) << script;
    EXPECT_TRUE(result.error().starts_with("TypeError") || result.error().starts_with("RangeError")) << result.error();
  }
}

TEST(WebAppTest, ResizeObserverReportsEmptyElements) {
  WebClient client;
  WebPage page = test::Navigate(
      client, Page("<script>log = []; const observer = new ResizeObserver(entries => log.push(entries.map(entry =>"
                   "[entry.target === document.body, entry.contentRect.width, entry.borderBoxSize[0].inlineSize,"
                   "entry instanceof ResizeObserverEntry])));"
                   "observer.observe(document.body); observer.observe(document.body);</script>"));
  EXPECT_EQ(Log(page), "[[[true,0,0,true]]]");
}

// The mutations are not reported, but the observers are created and check their options as in browsers.
TEST(WebAppTest, MutationObserverReportsNothing) {
  WebClient client;
  WebPage page = test::Navigate(
      client,
      Page("<p id=a>text</p><script>log = []; const observer = new MutationObserver(() => log.push('called'));"
           "observer.observe(document.body, {childList: true, subtree: true, attributes: true});"
           "observer.observe(document.getElementById('a').firstChild, {characterDataOldValue: true});"
           "observer.observe(document.body, {attributeFilter: ['id']});"
           "document.body.append(document.createElement('div')); document.body.id = 'x';"
           "document.getElementById('a').firstChild.data = 'changed';"
           "log.push(observer.takeRecords().length, observer.disconnect());</script>"),
      {.until = LoadState::NetworkIdle});
  EXPECT_EQ(Log(page), "[0,null]");
  for (const std::string_view script : {
           "new MutationObserver()",
           "MutationObserver(() => {})",
           "new MutationObserver(() => {}).observe({}, {childList: true})",
           "new MutationObserver(() => {}).observe(document.body)",
           "new MutationObserver(() => {}).observe(document.body, {subtree: true})",
           "new MutationObserver(() => {}).observe(document.body, {attributes: false, attributeOldValue: true})",
           "new MutationObserver(() => {}).observe(document.body, {attributes: false, attributeFilter: []})",
           "new MutationObserver(() => {}).observe(document, {characterData: false, characterDataOldValue: true})",
       }) {
    EXPECT_EQ(test::ErrorName(page, script), R"("TypeError")") << script;
  }
}

// The queries and the child lists are arrays: a script that checks for a NodeList or an HTMLCollection gets one.
TEST(WebAppTest, CollectionsAreArrays) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<p>a</p> <p>b</p>"));
  EXPECT_EQ(test::Eval(page,
                       "const body = document.body;"
                       "[document.querySelectorAll('p') instanceof NodeList, body.childNodes instanceof NodeList,"
                       "body.children instanceof HTMLCollection, body.childNodes instanceof HTMLCollection,"
                       "document.getElementsByTagName('p') instanceof HTMLCollection, [] instanceof NodeList,"
                       "[body, 1] instanceof NodeList, {length: 0} instanceof NodeList,"
                       "body.childNodes instanceof Array, NodeList.name, HTMLCollection.name]"),
            R"([true,true,true,false,true,true,false,false,true,"NodeList","HTMLCollection"])");
  EXPECT_EQ(test::ErrorName(page, "new NodeList()"), R"("TypeError")");
  EXPECT_EQ(test::ErrorName(page, "new HTMLCollection()"), R"("TypeError")");
  // The removal of the cookie banner of OneTrust, which tests for both.
  EXPECT_EQ(test::Eval(page,
                       "const remove = e => { if (e instanceof NodeList || e instanceof Array) {"
                       "for (let i = 0; i < e.length; i++) e[i].parentElement.removeChild(e[i]); } else {"
                       "e.parentElement.removeChild(e); } }; remove(document.querySelectorAll('p'));"
                       "document.querySelectorAll('p').length"),
            "0");
}

TEST(WebAppTest, LazyGlobalsCanBeReplaced) {
  WebClient client;
  WebPage page = test::Navigate(client, Page("<p></p>"));
  EXPECT_EQ(test::Eval(page, "window.matchMedia = () => 'mine'; window.Intl = 'replaced'; [matchMedia(), Intl]"),
            R"(["mine","replaced"])");
  EXPECT_EQ(test::Eval(page, "(1234.5).toLocaleString()"), R"("1,234.5")");
  // Before their first read: an HTML element interface too, and a strict mode assignment, as for any global.
  EXPECT_EQ(test::Eval(page,
                       "'use strict'; HTMLCanvasElement = 'canvas'; URL = 'url';"
                       "[HTMLCanvasElement, URL, Object.hasOwn(window, 'URL')]"),
            R"(["canvas","url",true])");
  // Until then, they are accessors of the object of the lazy globals, which a script can reach.
  EXPECT_EQ(test::Eval(page,
                       "const lazy = Object.getPrototypeOf(Window.prototype);"
                       "const { get, set } = Object.getOwnPropertyDescriptor(lazy, 'TextEncoder');"
                       "set.call(window); const own = Object.hasOwn(window, 'TextEncoder');"
                       "[own, window.TextEncoder, typeof get(), typeof lazy.TextEncoder]"),
            R"([true,null,"function","function"])");
  EXPECT_EQ(test::ErrorName(page, "Reflect.set(Object.getPrototypeOf(Window.prototype), 'TextDecoder', 1, 1)"),
            R"("TypeError")");
}

// --- Intl ---

class WebIntlTest : public test::PageTest {
 protected:
  WebIntlTest() : PageTest(Page("<p></p>")) {}
};

TEST_F(WebIntlTest, Globals) {
  EXPECT_EQ(eval("[typeof Intl, Object.prototype.toString.call(Intl), Object.keys(Intl).length,"
                 "Intl.getCanonicalLocales(['EN-us', 'fr-fr', 'en-US', 'zh-hant-tw', 'en-u-ca-gregory']),"
                 "Intl.NumberFormat.supportedLocalesOf(['fr', 'en-GB', 'en']), Intl.supportedValuesOf('calendar')]"),
            R"(["object","[object Intl]",0,["en-US","fr-FR","zh-Hant-TW","en-u-ca-gregory"],["en-GB","en"],)"
            R"(["gregory"]])");
  EXPECT_EQ(
      eval("const locale = new Intl.Locale('en-us-u-hc-h12'); const region = new Intl.Locale('fr', {region: 'CA'});"
           "[locale.toString(), locale.baseName, locale.language, locale.region, locale.script,"
           "region.toString(), locale.maximize().toString()]"),
      R"(["en-US-u-hc-h12","en-US","en","US",null,"fr-CA","en-US-u-hc-h12"])");
  EXPECT_EQ(errorName("Intl.getCanonicalLocales('not a tag')"), R"("RangeError")");
  EXPECT_EQ(errorName("Intl.getCanonicalLocales([1])"), R"("TypeError")");
  EXPECT_EQ(errorName("Intl.supportedValuesOf('nope')"), R"("RangeError")");
  EXPECT_EQ(errorName("new Intl.Locale()"), R"("TypeError")");
  // NumberFormat, DateTimeFormat and Collator can be called as functions, not the other constructors.
  EXPECT_EQ(eval("[Intl.NumberFormat('en').format(1e4), Intl.DateTimeFormat() instanceof Intl.DateTimeFormat,"
                 "new Intl.Collator() instanceof Intl.Collator, Intl.NumberFormat.name,"
                 "Intl.Collator.supportedLocalesOf('en-us')]"),
            R"(["10,000",true,true,"NumberFormat",["en-US"]])");
  EXPECT_EQ(errorName("Intl.PluralRules()"), R"("TypeError")");
}

TEST_F(WebIntlTest, NumberFormat) {
  EXPECT_EQ(eval("const f = (options, ...values) => values.map(new Intl.NumberFormat('en-US', options).format);"
                 "[f({}, 1234567.891, -0, 0.0005, NaN, -Infinity, 12n), f({maximumFractionDigits: 1}, 2.25, 1e21),"
                 "f({minimumFractionDigits: 2}, 5, 1.005), f({minimumIntegerDigits: 3}, 7),"
                 "f({maximumSignificantDigits: 3}, 1234.5, 0.012345), f({minimumSignificantDigits: 4}, 1.5),"
                 "f({useGrouping: false}, 12345), f({useGrouping: 'min2'}, 1234, 12345)]"),
            R"([["1,234,567.891","-0","0.001","NaN","-∞","12"],["2.3","1,000,000,000,000,000,000,000"],)"
            R"(["5.00","1.005"],["007"],["1,230","0.0123"],["1.500"],["12345"],["1234","12,345"]])");
  EXPECT_EQ(eval("[f({minimumSignificantDigits: 4}, 0.1, 0), f({maximumSignificantDigits: 2}, 99.9, 1e-7)]"),
            R"([["0.1000","0.000"],["100","0.0000001"]])");
  EXPECT_EQ(eval("[f({style: 'currency', currency: 'usd'}, 1234.5, -3), f({style: 'currency', currency: 'EUR'}, 1),"
                 "f({style: 'currency', currency: 'JPY'}, 1234.5),"
                 "f({style: 'currency', currency: 'CAD', currencyDisplay: 'narrowSymbol'}, 1),"
                 "f({style: 'currency', currency: 'USD', currencyDisplay: 'name'}, 1),"
                 "f({style: 'percent'}, 0.256, 1), f({style: 'percent', maximumFractionDigits: 1}, 0.2567),"
                 "f({style: 'unit', unit: 'kilometer-per-hour'}, 50), f({style: 'unit', unit: 'percent'}, 5),"
                 "f({style: 'unit', unit: 'liter', unitDisplay: 'long'}, 2)]"),
            R"([["$1,234.50","-$3.00"],["€1.00"],["¥1,235"],["$1.00"],["1.00 USD"],)"
            R"(["26%","100%"],["25.7%"],["50 km/h"],["5%"],["2 liter"]])");
  // A currency without symbol, or shown by its code, is separated by a no-break space.
  EXPECT_EQ(eval("[f({style: 'currency', currency: 'CHF'}, 1), f({style: 'currency', currency: 'USD', "
                 "currencyDisplay: 'code'}, 1)]"),
            "[[\"CHF\xC2\xA0"
            "1.00\"],[\"USD\xC2\xA0"
            "1.00\"]]");
  EXPECT_EQ(eval("[f({notation: 'compact'}, 999, 1234, 12345, 123456, 1234567, 999999, 2.5e9, 7e12, -1500),"
                 "f({notation: 'compact', compactDisplay: 'long'}, 1234), f({notation: 'scientific'}, 1234, 0.00012),"
                 "f({notation: 'engineering'}, 12345)]"),
            R"([["999","1.2K","12K","123K","1.2M","1M","2.5B","7T","-1.5K"],["1.2 thousand"],["1.234E3","1.2E-4"],)"
            R"(["12.345E3"]])");
  EXPECT_EQ(eval("[f({signDisplay: 'always'}, 5, -5, 0), f({signDisplay: 'exceptZero'}, 5, 0, -0.0001),"
                 "f({signDisplay: 'never'}, -5), f({signDisplay: 'negative'}, -5, -0)]"),
            R"([["+5","-5","+0"],["+5","0","0"],["5"],["-5","0"]])");
  EXPECT_EQ(eval("new Intl.NumberFormat('en-US', {style: 'currency', currency: 'USD'}).formatToParts(-1234.5)"
                 ".map(part => part.type + ':' + part.value).join(' ')"),
            R"("minusSign:- currency:$ integer:1 group:, integer:234 decimal:. fraction:50")");
  EXPECT_EQ(eval("const nf = new Intl.NumberFormat('fr-FR', {style: 'currency', currency: 'EUR'});"
                 "const options = nf.resolvedOptions(); [options.locale, options.minimumFractionDigits,"
                 "options.maximumFractionDigits, options.currency, options.useGrouping, nf.format === nf.format,"
                 "nf.formatRange(1, 2), new Intl.NumberFormat('en-GB').resolvedOptions().locale]"),
            R"(["en-US",2,2,"EUR","auto",true,"€1.00–€2.00","en-GB"])");
  EXPECT_EQ(eval("[(1234.5).toLocaleString(), (0.5).toLocaleString('de-DE', {style: 'percent'}),"
                 "(12).toLocaleString(undefined, {minimumFractionDigits: 1})]"),
            R"(["1,234.5","50%","12.0"])");
  EXPECT_EQ(errorName("new Intl.NumberFormat('en', {style: 'currency'})"), R"("TypeError")");
  EXPECT_EQ(errorName("new Intl.NumberFormat('en', {style: 'unit'})"), R"("TypeError")");
  EXPECT_EQ(errorName("new Intl.NumberFormat('en', {currency: 'dollars'})"), R"("RangeError")");
  EXPECT_EQ(errorName("new Intl.NumberFormat('en', {style: 'fancy'})"), R"("RangeError")");
  EXPECT_EQ(errorName("new Intl.NumberFormat('en', {maximumFractionDigits: 101})"), R"("RangeError")");
  EXPECT_EQ(errorName("new Intl.NumberFormat('en', {minimumFractionDigits: 3, maximumFractionDigits: 1})"),
            R"("RangeError")");
  EXPECT_EQ(errorName("new Intl.NumberFormat('en', {minimumSignificantDigits: 3, maximumSignificantDigits: 1})"),
            R"("RangeError")");
}

TEST_F(WebIntlTest, DateTimeFormat) {
  EXPECT_EQ(eval("const date = new Date(Date.UTC(2018, 0, 2, 23, 40, 40, 123));"
                 "const d = options => new Intl.DateTimeFormat('en-US', {timeZone: 'UTC', ...options}).format(date);"
                 "[d({}), d({dateStyle: 'full'}), d({dateStyle: 'long'}), d({dateStyle: 'medium'}),"
                 "d({dateStyle: 'short'}), d({timeStyle: 'full'}), d({timeStyle: 'long'}), d({timeStyle: 'medium'}),"
                 "d({timeStyle: 'short'}), d({dateStyle: 'long', timeStyle: 'short'}),"
                 "d({dateStyle: 'short', timeStyle: 'short'})]"),
            R"(["1/2/2018","Tuesday, January 2, 2018","January 2, 2018","Jan 2, 2018","1/2/18",)"
            R"("11:40:40 PM Coordinated Universal Time","11:40:40 PM UTC","11:40:40 PM","11:40 PM",)"
            R"("January 2, 2018 at 11:40 PM","1/2/18, 11:40 PM"])");
  EXPECT_EQ(eval("[d({month: 'long', year: 'numeric'}), d({month: 'short', day: 'numeric'}), d({weekday: 'short'}),"
                 "d({weekday: 'narrow', month: 'numeric', day: '2-digit'}), d({year: '2-digit'}), d({day: 'numeric'}),"
                 "d({hour: '2-digit', minute: '2-digit', hour12: false}), d({hour: 'numeric'}),"
                 "d({hourCycle: 'h23', hour: 'numeric', minute: 'numeric'}), d({minute: '2-digit', second: '2-digit',"
                 "fractionalSecondDigits: 3}), d({year: 'numeric', era: 'short'}), d({hour: 'numeric', timeZoneName:"
                 "'short'}), d({timeZone: 'Europe/Paris', hour: 'numeric', minute: 'numeric'})]"),
            R"(["January 2018","Jan 2","Tue","T, 1/02","18","2","23:40","11 PM","23:40","40:40.123","2018 AD",)"
            R"("11 PM UTC","11:40 PM"])");
  EXPECT_EQ(eval("new Intl.DateTimeFormat('en-US', {timeZone: 'utc', dateStyle: 'medium', timeStyle: 'short'})"
                 ".formatToParts(date).map(part => part.type).join()"),
            R"("month,literal,day,literal,year,literal,hour,literal,minute,literal,dayPeriod")");
  EXPECT_EQ(eval("const options = new Intl.DateTimeFormat('en', {timeZone: 'Etc/UTC', hour: 'numeric'})"
                 ".resolvedOptions(); [options.timeZone, options.locale, options.calendar, options.hourCycle,"
                 "options.hour12, options.hour, new Intl.DateTimeFormat().resolvedOptions().year]"),
            R"(["UTC","en","gregory","h12",true,"numeric","numeric"])");
  EXPECT_EQ(eval("[date.toLocaleDateString('en-US', {timeZone: 'UTC'}), date.toLocaleTimeString('en-US', {timeZone: "
                 "'UTC'}), date.toLocaleString('en-US', {timeZone: 'UTC'}), date.toLocaleDateString(undefined, "
                 "{timeZone: 'UTC', month: 'long'}), new Intl.DateTimeFormat('en', {timeZone: 'UTC'})"
                 ".formatRange(date, date)]"),
            R"(["1/2/2018","11:40:40 PM","1/2/2018, 11:40:40 PM","January","1/2/2018 – 1/2/2018"])");
  // Without time zone, the local time of the process, as Date does.
  EXPECT_EQ(
      eval("new Intl.DateTimeFormat('en-US', {hour: 'numeric', minute: '2-digit', hour12: false}).format(date) ==="
           "String(date.getHours()).padStart(2, '0') + ':' + String(date.getMinutes()).padStart(2, '0')"),
      "true");
  EXPECT_EQ(errorName("new Intl.DateTimeFormat('en', {timeZone: 'Not a zone!'})"), R"("RangeError")");
  EXPECT_EQ(errorName("new Intl.DateTimeFormat('en', {dateStyle: 'long', year: 'numeric'})"), R"("TypeError")");
  EXPECT_EQ(errorName("new Intl.DateTimeFormat('en').format(NaN)"), R"("RangeError")");
  EXPECT_EQ(errorName("new Intl.DateTimeFormat('en', {month: 'big'})"), R"("RangeError")");
}

TEST_F(WebIntlTest, PluralRulesCollatorRelativeTimesAndLists) {
  EXPECT_EQ(eval("const cardinal = new Intl.PluralRules('en-US'); const ordinal = new Intl.PluralRules('en-US', "
                 "{type: 'ordinal'}); [[0, 1, 2, 1.5, -1].map(n => cardinal.select(n)), [1, 2, 3, 4, 11, 12, 13, 21, "
                 "22, 23, 112, 1.5].map(n => ordinal.select(n)), new Intl.PluralRules('en', {minimumFractionDigits: "
                 "1}).select(1), ordinal.resolvedOptions().pluralCategories, cardinal.selectRange(1, 2)]"),
            R"([["other","one","other","other","one"],["one","two","few","other","other","other","other","one",)"
            R"("two","few","other","other"],"other",["few","one","two","other"],"other"])");
  EXPECT_EQ(eval("[['b', 'a', 'B', 'á', '10', '9'].sort(new Intl.Collator().compare),"
                 "['10', '9', '2', 'a10', 'a9'].sort(new Intl.Collator('en', {numeric: true}).compare),"
                 "new Intl.Collator('en', {sensitivity: 'base'}).compare('a', 'Á'),"
                 "new Intl.Collator('en', {sensitivity: 'accent'}).compare('a', 'A'),"
                 "new Intl.Collator('en', {sensitivity: 'accent'}).compare('a', 'á'),"
                 "new Intl.Collator('en', {sensitivity: 'case'}).compare('a', 'á'),"
                 "new Intl.Collator('en', {caseFirst: 'upper'}).compare('a', 'A'),"
                 "new Intl.Collator('en', {ignorePunctuation: true}).compare('a-b', 'ab'),"
                 "'a'.localeCompare('B'), 'b'.localeCompare('a'), 'a'.localeCompare('a')]"),
            R"([["10","9","a","á","b","B"],["2","9","10","a9","a10"],0,0,-1,0,1,0,-1,1,0])");
  // localeCompare() compares two ASCII strings natively, in the order of the Collator.
  EXPECT_EQ(eval("const words = ['b', 'B', 'a', 'A', 'ab', 'Ab', 'aB', '', 'a-b', 'a b', '_', '10', '9', 'Z', 'z'];"
                 "const collator = new Intl.Collator();"
                 "[words.slice().sort((x, y) => x.localeCompare(y)).join() === words.slice().sort(collator.compare)"
                 ".join(), words.slice().sort((x, y) => x.localeCompare(y)).join(), 'é'.localeCompare('f'),"
                 "'a'.localeCompare('A', undefined, {caseFirst: 'upper'}), 'a'.localeCompare('A', 'en'),"
                 "'ab'.localeCompare('a'), new String('a').localeCompare('B'), 'x'.localeCompare(),"
                 "String.prototype.localeCompare.call(1, 2), 'a'.localeCompare('B', undefined, undefined),"
                 "'a'.localeCompare('é'), 'é'.localeCompare('a')]"),
            R"([true,",10,9,_,a,A,a b,a-b,ab,aB,Ab,b,B,z,Z",-1,1,-1,1,-1,1,-1,-1,-1,1])");
  EXPECT_EQ(
      eval("const rtf = new Intl.RelativeTimeFormat('en'); const auto = new Intl.RelativeTimeFormat('en', "
           "{numeric: 'auto'}); const short = new Intl.RelativeTimeFormat('en', {style: 'short'});"
           "[rtf.format(-1, 'day'), rtf.format(2, 'hours'), rtf.format(1000, 'year'), rtf.format(-0, 'second'),"
           "auto.format(-1, 'day'), auto.format(0, 'day'), auto.format(1, 'day'), auto.format(1, 'year'),"
           "auto.format(-1, 'month'), auto.format(0, 'second'), auto.format(3, 'week'), short.format(3, 'minute'),"
           "short.format(-2, 'quarter'), rtf.formatToParts(1200, 'day').map(part => part.type).join()]"),
      R"(["1 day ago","in 2 hours","in 1,000 years","0 seconds ago","yesterday","today","tomorrow",)"
      R"("next year","last month","now","in 3 weeks","in 3 min.","2 qtrs. ago","literal,integer,group,integer,)"
      R"(literal"])");
  EXPECT_EQ(eval("const list = options => new Intl.ListFormat('en', options);"
                 "[list().format(['a', 'b', 'c']), list().format(['a', 'b']), list().format(['a']), list().format([]),"
                 "list({type: 'disjunction'}).format(['a', 'b', 'c']), list({style: 'short'}).format(['a', 'b', 'c']),"
                 "list({style: 'narrow'}).format(['a', 'b', 'c']), list({type: 'unit', style: 'narrow'})"
                 ".format(['a', 'b']), list().formatToParts(['a', 'b']).map(part => part.type).join()]"),
            R"(["a, b, and c","a and b","a","","a, b, or c","a, b, & c","a, b, c","a b","element,literal,element"])");
  EXPECT_EQ(errorName("new Intl.RelativeTimeFormat('en').format(1, 'fortnight')"), R"("RangeError")");
  EXPECT_EQ(errorName("new Intl.RelativeTimeFormat('en').format(Infinity, 'day')"), R"("RangeError")");
  EXPECT_EQ(errorName("new Intl.ListFormat('en').format([1])"), R"("TypeError")");
  EXPECT_EQ(errorName("new Intl.PluralRules('en', {type: 'big'})"), R"("RangeError")");
  EXPECT_EQ(errorName("new Intl.Collator('en', {sensitivity: 'none'})"), R"("RangeError")");
}

}  // namespace
}  // namespace aeronet
