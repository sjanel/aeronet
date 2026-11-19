// The DOM bindings of a page, through WebPage::evaluate(): wrappers, navigation, text nodes, mutations, queries, the
// errors of the DOM, and the helpers of the prelude (events, classList, dataset...).
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"
#include "web-test-util.hpp"

namespace aeronet {
namespace {

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gServer = test::MakePageServer();

constexpr std::string_view kPage =
    "<!DOCTYPE html><html><head><title> The \n title </title><base href=\"/static/\"></head><body>"
    "<div id=\"root\" class=\"a b\" data-user-id=\"7\"><p id=\"first\">one<!--note--><b>bold</b></p>"
    "<p id=\"second\" class=\"b\">two</p></div>"
    "<template id=\"tpl\"><i>in template</i>text</template>"
    "<noscript><p id=\"hidden\">no js</p></noscript>"
    "<svg><foreignObject id=\"fo\"></foreignObject></svg></body></html>";

class WebDomTest : public ::testing::Test {
 protected:
  // The JSON result of 'script', which must not throw.
  std::string eval(std::string_view script) { return test::Eval(page, script); }

  // The name of the error thrown by 'script'.
  std::string errorName(std::string_view script) {
    return eval("(() => { try { " + std::string(script) + "; return 'no error'; } catch (e) { return e.name; } })()");
  }

  WebClient client;
  WebPage page{test::Navigate(client, test::PageUrl(gServer, kPage))};
};

TEST_F(WebDomTest, WrapperIdentityAndExpandos) {
  EXPECT_EQ(eval("document.body === document.body"), "true");
  EXPECT_EQ(eval("document.getElementById('first') === document.querySelector('#root > p')"), "true");
  EXPECT_EQ(eval("document.documentElement.parentNode === document"), "true");
  EXPECT_EQ(eval("document.getElementById('first').firstChild === document.getElementById('first').childNodes[0]"),
            "true");
  // A property added by a script stays on the node, also in a WeakMap.
  EXPECT_EQ(eval("const weak = new WeakMap([[document.body, 'w']]); document.body.custom = {n: 1}; 0"), "0");
  EXPECT_EQ(eval("[document.body.custom.n, weak.get(document.body)]"), R"([1,"w"])");
}

TEST_F(WebDomTest, NodeTypesAndNames) {
  EXPECT_EQ(eval("[document.nodeType, document.nodeName]"), R"([9,"#document"])");
  EXPECT_EQ(eval("[document.firstChild.nodeType, document.firstChild.nodeName]"), R"([10,"html"])");
  EXPECT_EQ(eval("const p = document.getElementById('first'); [p.nodeType, p.nodeName, p.tagName, p.localName]"),
            R"([1,"P","P","p"])");
  EXPECT_EQ(eval("[p.firstChild.nodeType, p.firstChild.nodeName, p.childNodes[1].nodeType, p.childNodes[1].nodeName]"),
            R"([3,"#text",8,"#comment"])");
  // The case of the SVG elements is kept.
  EXPECT_EQ(eval("[document.getElementById('fo').tagName, document.getElementById('fo') instanceof HTMLElement]"),
            R"(["foreignObject",false])");
  EXPECT_EQ(eval("const content = document.getElementById('tpl').content; [content.nodeType, content.nodeName]"),
            R"([11,"#document-fragment"])");
  EXPECT_EQ(eval("[p instanceof HTMLElement, p instanceof Element, p instanceof Node, p instanceof EventTarget,"
                 "p.firstChild instanceof Text, p.firstChild instanceof CharacterData, p.childNodes[1] instanceof "
                 "Comment, document instanceof Document, window instanceof Window]"),
            "[true,true,true,true,true,true,true,true,true]");
  EXPECT_EQ(eval("[Node.ELEMENT_NODE, Node.TEXT_NODE, p.COMMENT_NODE, Node.DOCUMENT_NODE]"), "[1,3,8,9]");
  EXPECT_EQ(eval("document.getElementById('root').content"), "");
  EXPECT_EQ(eval("const meta = document.createElement('meta'); meta.setAttribute('content', 'c');"
                 "[meta.content, document.getElementById('fo').content]"),
            R"(["c",null])");
}

TEST_F(WebDomTest, TreeNavigation) {
  eval(
      "var root = document.getElementById('root'); var first = document.getElementById('first');"
      "var second = document.getElementById('second'); 0");
  EXPECT_EQ(eval("[first.parentNode === root, first.parentElement === root, document.documentElement.parentElement]"),
            "[true,true,null]");
  EXPECT_EQ(eval("[root.childNodes.length, root.firstChild === first, root.lastChild === second]"), "[2,true,true]");
  EXPECT_EQ(eval("[first.nextSibling === second, second.previousSibling === first, first.previousSibling, "
                 "second.nextSibling]"),
            "[true,true,null,null]");
  EXPECT_EQ(eval("[first.firstChild.nextElementSibling.tagName, first.lastChild.previousElementSibling, "
                 "second.previousElementSibling === first, first.nextElementSibling === second]"),
            R"(["B",null,true,true])");
  EXPECT_EQ(eval("[root.children.length, root.firstElementChild === first, root.lastElementChild === second, "
                 "root.childElementCount, document.childElementCount, document.children[0].tagName]"),
            R"([2,true,true,2,1,"HTML"])");
  EXPECT_EQ(eval("[first.ownerDocument === document, document.ownerDocument, first.isConnected, "
                 "document.createElement('p').isConnected]"),
            "[true,null,true,false]");
  EXPECT_EQ(eval("[root.contains(first.firstChild), root.contains(root), first.contains(root), root.contains(null), "
                 "root.hasChildNodes(), first.firstChild.hasChildNodes()]"),
            "[true,true,false,false,true,false]");
  EXPECT_EQ(eval("document.firstElementChild === document.documentElement && "
                 "document.lastElementChild === document.documentElement"),
            "true");
}

TEST_F(WebDomTest, TextAndCommentNodes) {
  eval("var text = document.getElementById('first').firstChild; var comment = text.nextSibling; 0");
  EXPECT_EQ(eval("[text.data, text.nodeValue, text.textContent, text.length, comment.data, comment.length]"),
            R"(["one","one","one",3,"note",4])");
  // length counts UTF-16 code units.
  EXPECT_EQ(eval("text.data = 'é😀'; [text.data, text.length, document.getElementById('first').textContent]"),
            R"(["é😀",3,"é😀bold"])");
  EXPECT_EQ(eval("comment.nodeValue = 'changed'; comment.textContent = 'again'; comment.data"), R"("again")");
  EXPECT_EQ(eval("text.data = null; text.data"), R"("")");
  EXPECT_EQ(eval("[document.body.nodeValue, document.nodeValue]"), "[null,null]");
  EXPECT_EQ(eval("document.body.nodeValue = 'ignored'; document.body.nodeValue"), "null");
  EXPECT_EQ(eval("const created = document.createTextNode('new'); [created.data, created.parentNode, "
                 "document.createComment('c').nodeType, document.createTextNode('').length]"),
            R"(["new",null,8,0])");
  EXPECT_EQ(eval("text.remove(); [text.parentNode, document.getElementById('first').firstChild === comment]"),
            "[null,true]");
}

TEST_F(WebDomTest, TextContent) {
  EXPECT_EQ(eval("document.getElementById('root').textContent"), R"("oneboldtwo")");
  EXPECT_EQ(eval("[document.textContent, document.firstChild.textContent, "
                 "document.getElementById('tpl').content.textContent]"),
            R"([null,null,""])");
  EXPECT_EQ(eval("const root = document.getElementById('root'); root.textContent = '<b>x</b>';"
                 "[root.innerHTML, root.childNodes.length, document.getElementById('first')]"),
            R"(["&lt;b&gt;x&lt;/b&gt;",1,null])");
  EXPECT_EQ(eval("root.textContent = null; root.childNodes.length"), "0");
  EXPECT_EQ(eval("document.textContent = 'ignored'; document.body !== null"), "true");
}

TEST_F(WebDomTest, InnerAndOuterHtml) {
  EXPECT_EQ(eval("document.getElementById('second').outerHTML"), R"("<p id=\"second\" class=\"b\">two</p>")");
  EXPECT_EQ(eval("document.getElementById('first').innerHTML"), R"("one<!--note--><b>bold</b>")");
  EXPECT_EQ(eval("const second = document.getElementById('second'); second.innerHTML = '<i>x</i><u>y</u>';"
                 "[second.children.length, second.firstChild.tagName]"),
            R"([2,"I"])");
  EXPECT_EQ(eval("second.innerHTML = null; second.innerHTML"), R"("")");
  EXPECT_EQ(eval("document.getElementById('tpl').innerHTML"), R"("<i>in template</i>text")");
}

TEST_F(WebDomTest, InsertAdjacent) {
  EXPECT_EQ(eval("const first = document.getElementById('first');"
                 "first.insertAdjacentHTML('beforebegin', '<a>1</a>'); first.insertAdjacentHTML('AfterBegin', '2');"
                 "first.insertAdjacentHTML('beforeend', '<a>3</a>'); first.insertAdjacentHTML('afterend', '4');"
                 "document.getElementById('root').innerHTML"),
            R"("<a>1</a><p id=\"first\">2one<!--note--><b>bold</b><a>3</a></p>4<p id=\"second\" class=\"b\">two</p>")");
  EXPECT_EQ(errorName("first.insertAdjacentHTML('middle', 'x')"), R"("SyntaxError")");
  EXPECT_EQ(errorName("document.documentElement.insertAdjacentHTML('beforebegin', 'x')"),
            R"("NoModificationAllowedError")");
  EXPECT_EQ(errorName("document.createElement('p').insertAdjacentHTML('afterend', 'x')"),
            R"("NoModificationAllowedError")");

  EXPECT_EQ(eval("const second = document.getElementById('second'); "
                 "[first.insertAdjacentElement('beforebegin', second) === second, first.previousSibling === second]"),
            "[true,true]");
  EXPECT_EQ(eval("document.createElement('p').insertAdjacentElement('afterend', second)"), "null");
  EXPECT_EQ(errorName("first.insertAdjacentElement('afterbegin', document.getElementById('root'))"),
            R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("first.insertAdjacentElement('afterbegin', first.firstChild)"), R"("TypeError")");

  EXPECT_EQ(eval("first.insertAdjacentText('afterbegin', '<t>'); first.firstChild.data"), R"("<t>")");
  EXPECT_EQ(eval("document.createElement('p').insertAdjacentText('beforebegin', 'x'); 'ignored'"), R"("ignored")");

  // Beside the root element is in the document, which has one element and no text (DOM "insert adjacent").
  const std::string before = eval("document.documentElement.outerHTML");
  EXPECT_EQ(errorName("document.documentElement.insertAdjacentElement('beforebegin', document.createElement('p'))"),
            R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("document.documentElement.insertAdjacentElement('afterend', document.documentElement)"),
            R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("document.documentElement.insertAdjacentText('afterend', 'x')"), R"("HierarchyRequestError")");
  EXPECT_EQ(eval("document.documentElement.outerHTML"), before);
}

// Beside an element of the contents of a template: in its document fragment.
TEST_F(WebDomTest, InsertAdjacentInTemplateContents) {
  EXPECT_EQ(eval("const content = document.getElementById('tpl').content; const italic = content.firstChild;"
                 "italic.insertAdjacentHTML('beforebegin', '<b>1</b>'); italic.insertAdjacentText('afterend', '2');"
                 "[italic.insertAdjacentElement('afterend', document.createElement('u')).nodeName, "
                 "document.getElementById('tpl').innerHTML]"),
            R"(["U","<b>1</b><i>in template</i><u></u>2text"])");
}

// A document has at most one element and one doctype, the doctype first (DOM "ensure pre-insertion validity").
TEST_F(WebDomTest, ChildrenOfTheDocument) {
  EXPECT_EQ(errorName("document.appendChild(document.createElement('p'))"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("document.insertBefore(document.documentElement, document.firstChild)"),
            R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("document.body.appendChild(document.firstChild)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("document.appendChild(document.firstChild)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("document.appendChild(document.getElementById('tpl').content)"), R"("HierarchyRequestError")");
  EXPECT_EQ(eval("[document.childNodes.length, document.firstChild.nodeType, document.lastChild.nodeName]"),
            R"([2,10,"HTML"])");
  // The element can be replaced, by an element or by a fragment of one element.
  EXPECT_EQ(eval("const content = document.getElementById('tpl').content, html = document.documentElement;"
                 "const other = document.createElement('html');"
                 "document.replaceChild(other, html) === html && document.documentElement === other"),
            "true");
  EXPECT_EQ(eval("content.lastChild.remove(); document.replaceChild(content, other) === other && "
                 "document.documentElement.nodeName"),
            R"("I")");
  // Once removed, an element can be added again, but not before the doctype; and the doctype not after the element.
  EXPECT_EQ(eval("document.documentElement.remove(); document.appendChild(html) === html"), "true");
  EXPECT_EQ(eval("html.remove(); const doctype = document.removeChild(document.firstChild);"
                 "document.appendChild(html); [document.childNodes.length]"),
            "[1]");
  EXPECT_EQ(errorName("document.appendChild(doctype)"), R"("HierarchyRequestError")");
  EXPECT_EQ(eval("document.insertBefore(doctype, html) === doctype && document.firstChild === doctype"), "true");
  EXPECT_EQ(eval("html.remove(); document.insertBefore(document.createComment('c'), doctype); "
                 "document.appendChild(html) === html"),
            "true");
  EXPECT_EQ(errorName("html.remove(); document.insertBefore(document.createElement('p'), doctype)"),
            R"("HierarchyRequestError")");
  EXPECT_EQ(eval("[...document.childNodes].map(node => node.nodeType)"), "[8,10]");
}

TEST_F(WebDomTest, Attributes) {
  eval("var root = document.getElementById('root'); 0");
  EXPECT_EQ(eval("[root.id, root.className, root.getAttribute('data-user-id'), root.getAttribute('missing'), "
                 "root.hasAttribute('class'), root.hasAttribute('missing'), root.getAttributeNames()]"),
            R"(["root","a b","7",null,true,false,["id","class","data-user-id"]])");
  EXPECT_EQ(eval("root.setAttribute('Title', 't'); root.id = 'renamed'; root.className = 'c';"
                 "root.removeAttribute('data-user-id'); root.removeAttribute('missing'); root.outerHTML.split('>')[0]"),
            R"("<div id=\"renamed\" class=\"c\" title=\"t\"")");
  EXPECT_EQ(errorName("root.setAttribute('a b', 'x')"), R"("InvalidCharacterError")");
  EXPECT_EQ(errorName("root.setAttribute('only-name')"), R"("TypeError")");
}

TEST_F(WebDomTest, CreateAndMoveNodes) {
  EXPECT_EQ(eval("const root = document.getElementById('root'); const first = document.getElementById('first');"
                 "const second = document.getElementById('second'); const span = document.createElement('SPAN');"
                 "[root.appendChild(span) === span, root.lastChild === span, span.tagName]"),
            R"([true,true,"SPAN"])");
  EXPECT_EQ(eval("root.insertBefore(span, first); root.firstChild === span"), "true");
  EXPECT_EQ(eval("root.insertBefore(span, span); root.firstChild === span"), "true");
  EXPECT_EQ(eval("root.insertBefore(document.createTextNode('t'), null); root.lastChild.data"), R"("t")");
  EXPECT_EQ(eval("root.insertBefore(second, undefined); root.lastChild === second"), "true");
  EXPECT_EQ(eval("[root.removeChild(span) === span, span.parentNode, root.firstChild === first]"), "[true,null,true]");
  EXPECT_EQ(eval("const replaced = root.replaceChild(span, first); [replaced === first, root.firstChild === span]"),
            "[true,true]");
  EXPECT_EQ(eval("root.replaceChild(span, span) === span && root.firstChild === span"), "true");
  // The node replaces its previous sibling.
  EXPECT_EQ(eval("root.insertBefore(first, span); root.replaceChild(span, first); root.firstChild === span"), "true");
  EXPECT_EQ(eval("span.remove(); span.remove(); [span.parentNode, root.contains(span)]"), "[null,false]");
  // A document fragment brings its children.
  EXPECT_EQ(eval("const content = document.getElementById('tpl').content; root.appendChild(content);"
                 "[content.childNodes.length, root.lastChild.data, root.lastChild.previousSibling.tagName]"),
            R"([0,"text","I"])");
  EXPECT_EQ(eval("document.createElement('my-element').localName"), R"("my-element")");
  EXPECT_EQ(errorName("document.createElement('1x')"), R"("InvalidCharacterError")");
}

TEST_F(WebDomTest, InsertionErrors) {
  eval("var root = document.getElementById('root'); var first = document.getElementById('first'); 0");
  EXPECT_EQ(errorName("first.appendChild(root)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("root.appendChild(root)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("first.firstChild.appendChild(document.createElement('p'))"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("document.appendChild(document.createTextNode('x'))"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("root.appendChild(document)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("root.insertBefore(document.createElement('p'), first.firstChild)"), R"("NotFoundError")");
  EXPECT_EQ(errorName("root.removeChild(first.firstChild)"), R"("NotFoundError")");
  EXPECT_EQ(errorName("root.replaceChild(document.createElement('p'), first.firstChild)"), R"("NotFoundError")");
  EXPECT_EQ(errorName("root.replaceChild(root, first)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("root.appendChild({})"), R"("TypeError")");
  EXPECT_EQ(errorName("root.appendChild()"), R"("TypeError")");
  EXPECT_EQ(errorName("root.insertBefore(first)"), R"("TypeError")");
  // Nothing changed.
  EXPECT_EQ(eval("root.childNodes.length"), "2");
}

TEST_F(WebDomTest, Queries) {
  EXPECT_EQ(eval("[document.querySelectorAll('p').length, document.querySelector('.b').id, "
                 "document.querySelector('nothing'), document.getElementById('root').querySelectorAll('b').length, "
                 "document.getElementById('first').querySelector('p')]"),
            R"([2,"root",null,1,null])");
  EXPECT_EQ(errorName("document.querySelector('[')"), R"("SyntaxError")");
  EXPECT_EQ(errorName("document.body.querySelectorAll('p:unknown')"), R"("SyntaxError")");
  EXPECT_EQ(eval("[document.getElementById('second').textContent, document.getElementById('missing'), "
                 "document.getElementById('')]"),
            R"(["two",null,null])");
  EXPECT_EQ(eval("[document.getElementsByTagName('P').length, document.getElementsByTagName('*').length, "
                 "document.getElementsByTagName('foreignObject').length, "
                 "document.getElementsByTagName('foreignobject').length, "
                 "document.getElementById('root').getElementsByTagName('b')[0].textContent]"),
            R"([2,13,1,0,"bold"])");
  EXPECT_EQ(eval("[document.getElementsByClassName('b').length, document.getElementsByClassName(' b  a ').length, "
                 "document.getElementsByClassName('').length, document.body.getElementsByClassName('a')[0].id]"),
            R"([2,1,0,"root"])");
  // Whole tokens only, separated by ASCII whitespace (not the vertical tab), each name required, duplicates allowed.
  EXPECT_EQ(eval("['\\t\\n\\f\\r ', 'b b', 'a c', 'a\\tb', 'ab', 'a\\vb', 'bb', ' a'].map("
                 "names => document.getElementsByClassName(names).length)"),
            "[0,2,0,1,0,0,0,1]");
}

TEST_F(WebDomTest, Document) {
  EXPECT_EQ(eval("[document.documentElement.tagName, document.head.tagName, document.body.tagName]"),
            R"(["HTML","HEAD","BODY"])");
  EXPECT_EQ(eval("[document.title, document.readyState, document.characterSet, document.currentScript, "
                 "document.defaultView === window, document.location === location, document.URL === location.href, "
                 "document.documentURI === location.href, document.compatMode, document.hidden, "
                 "document.visibilityState, document.contentType, document.referrer, document.hasFocus(), "
                 "document.activeElement === document.body, document.domain]"),
            R"(["The title","complete","UTF-8",null,true,true,true,true,"CSS1Compat",false,"visible","text/html",)"
            R"("",true,true,"127.0.0.1"])");
  EXPECT_EQ(eval("document.title = 'New'; document.title"), R"("New")");
  EXPECT_EQ(eval("document.querySelector('title').remove(); document.title = 'none'; document.title"), R"("")");
}

TEST_F(WebDomTest, NoscriptIsText) {
  EXPECT_EQ(eval("const noscript = document.querySelector('noscript'); [noscript.childNodes.length, "
                 "noscript.firstChild.nodeType, document.getElementById('hidden'), noscript.textContent]"),
            R"([1,3,null,"<p id=\"hidden\">no js</p>"])");
  EXPECT_FALSE(page.document().querySelector("#hidden"));
  // As for the fragments parsed in it.
  EXPECT_EQ(eval("document.body.insertAdjacentHTML('beforeend', '<noscript><b>x</b></noscript>');"
                 "document.body.lastChild.firstChild.nodeType"),
            "3");
}

TEST_F(WebDomTest, IllegalInvocations) {
  EXPECT_EQ(errorName("new Node()"), R"("TypeError")");
  EXPECT_EQ(errorName("HTMLElement()"), R"("TypeError")");
  EXPECT_EQ(errorName("Object.getOwnPropertyDescriptor(Node.prototype, 'nodeType').get.call({})"), R"("TypeError")");
  const std::string text = "document.getElementById('first').firstChild";
  EXPECT_EQ(errorName("Element.prototype.getAttribute.call(" + text + ", 'id')"), R"("TypeError")");
  EXPECT_EQ(errorName("Object.getOwnPropertyDescriptor(CharacterData.prototype, 'data').get.call(document.body)"),
            R"("TypeError")");
  EXPECT_EQ(errorName("Document.prototype.getElementById.call(document.body, 'x')"), R"("TypeError")");
  EXPECT_EQ(errorName("Element.prototype.querySelector.call(" + text + ", 'p')"), R"("TypeError")");
  EXPECT_EQ(errorName("document.createElement()"), R"("TypeError")");
  EXPECT_EQ(errorName("document.getElementById(Symbol())"), R"("TypeError")");
}

// The exception of the conversion of an argument or a value (toString()) propagates out of every binding, which then
// changes nothing.
TEST_F(WebDomTest, ConversionErrorsPropagate) {
  const std::string before = eval("document.documentElement.outerHTML");
  EXPECT_EQ(eval(R"(
    const boom = { toString() { throw new Error('boom') } };
    const element = document.getElementById('second'), text = element.firstChild;
    [
      () => element.textContent = boom, () => text.textContent = boom, () => text.nodeValue = boom,
      () => text.data = boom, () => element.querySelector(boom), () => document.querySelectorAll(boom),
      () => element.getElementsByTagName(boom), () => element.getElementsByClassName(boom), () => element.id = boom,
      () => element.className = boom, () => element.getAttribute(boom), () => element.setAttribute(boom, 'v'),
      () => element.setAttribute('n', boom), () => element.removeAttribute(boom), () => element.hasAttribute(boom),
      () => element.innerHTML = boom, () => element.insertAdjacentHTML(boom, ''),
      () => element.insertAdjacentHTML('beforeend', boom), () => element.insertAdjacentElement(boom, element),
      () => element.insertAdjacentText(boom, ''), () => element.insertAdjacentText('beforeend', boom),
      () => document.getElementById(boom), () => document.createElement(boom), () => document.createTextNode(boom),
      () => document.createComment(boom), () => document.cookie = boom, () => document.write(boom),
      () => document.writeln('a', boom),
    ].map((call, index) => { try { call(); return index + ': no error'; } catch (e) { return e.message; } })
     .filter(message => message !== 'boom'))"),
            "[]");
  EXPECT_EQ(eval("document.documentElement.outerHTML"), before);
}

TEST_F(WebDomTest, WrongArguments) {
  EXPECT_EQ(errorName("document.body.contains({})"), R"("TypeError")");
  EXPECT_EQ(errorName("document.body.insertBefore({}, null)"), R"("TypeError")");
  EXPECT_EQ(errorName("document.body.replaceChild({}, document.body.firstChild)"), R"("TypeError")");
  EXPECT_EQ(errorName("document.body.insertAdjacentElement('afterbegin')"), R"("TypeError")");
  EXPECT_EQ(errorName("document.body.insertAdjacentElement('afterbegin', document.createTextNode('t'))"),
            R"("TypeError")");
  EXPECT_EQ(errorName("document.body.insertAdjacentElement('afterbegin', document.documentElement)"),
            R"("HierarchyRequestError")");
  EXPECT_EQ(eval("document.createElement('p').insertAdjacentElement('afterend', document.createElement('i'))"), "null");
}

TEST_F(WebDomTest, PreludeElementHelpers) {
  EXPECT_EQ(eval("const list = document.getElementById('root').classList; list.add('c', 'a'); list.remove('b');"
                 "[list.value, list.length, list.item(0), list.item(5), list.contains('c'), list.toggle('a'), "
                 "list.toggle('z', true), list.toggle('z', true), list.toggle('y', false), list.replace('z', 'c'), "
                 "list.replace('q', 'r'), [...list], String(list), list.supports('x')]"),
            R"(["a c",2,"a",null,true,false,true,true,false,true,false,["c"],"c",true])");
  EXPECT_EQ(errorName("list.add('')"), R"("SyntaxError")");
  EXPECT_EQ(errorName("list.add('a b')"), R"("InvalidCharacterError")");
  EXPECT_EQ(eval("const seen = []; list.forEach((token, i) => seen.push(token + i)); list.value = 'v w';"
                 "[seen, [...list.keys()], [...list.entries()], document.getElementById('root').className]"),
            R"([["c0"],[0,1],[[0,"v"],[1,"w"]],"v w"])");
  EXPECT_EQ(eval("document.getElementById('root').classList = 'x'; document.getElementById('root').className"),
            R"("x")");

  EXPECT_EQ(eval("const data = document.getElementById('root').dataset; data.someValue = 5; delete data.userId;"
                 "[data.someValue, data.userId, 'someValue' in data, Object.keys(data), "
                 "document.getElementById('root').getAttribute('data-some-value'), data[Symbol.iterator]]"),
            R"(["5",null,true,["someValue"],"5",null])");
  EXPECT_EQ(eval("JSON.stringify(Object.getOwnPropertyDescriptor(data, 'someValue')) + "
                 "Object.getOwnPropertyDescriptor(data, 'none')"),
            R"("{\"value\":\"5\",\"writable\":true,\"enumerable\":true,\"configurable\":true}undefined")");

  EXPECT_EQ(eval("const style = document.body.style; style.display = 'none'; style.setProperty('color', 'red');"
                 "style.setProperty('empty', null); [style.display, style.getPropertyValue('color'), "
                 "style.removeProperty('color'), style.getPropertyValue('missing'), style.cssText, "
                 "document.body.style === style]"),
            R"(["none","red","red","","display: none; empty: ;",true])");
  EXPECT_EQ(eval("style.cssText = 'x'; document.body.style = 'color: blue'; document.body.getAttribute('style')"),
            R"("color: blue")");
  EXPECT_EQ(eval("document.getElementById('second').attributes"),
            R"([{"name":"id","localName":"id","value":"second"},{"name":"class","localName":"class","value":"b"}])");

  // Reflected attributes, URLs resolved against the base URL.
  EXPECT_EQ(eval("const link = document.createElement('a'); link.href = 'page.html'; link.title = 't';"
                 "link.hidden = true; [link.href.endsWith('/static/page.html'), link.getAttribute('href'), "
                 "link.title, link.hidden, link.src, link.name]"),
            R"([true,"page.html","t",true,"",""])");
  EXPECT_EQ(eval("link.hidden = false; link.href = 'http://[bad'; [link.hasAttribute('hidden'), link.href]"),
            R"([false,"http://[bad"])");
}

TEST_F(WebDomTest, PreludeChildAndParentNodeHelpers) {
  EXPECT_EQ(eval("const root = document.getElementById('root'); const first = document.getElementById('first');"
                 "root.replaceChildren('a', document.createElement('i')); root.append('b', 1); root.prepend('0');"
                 "root.innerHTML"),
            R"("0a<i></i>b1")");
  EXPECT_EQ(eval("const i = root.querySelector('i'); i.before('<'); i.after('>', '!'); i.replaceWith('I');"
                 "[root.textContent, i.parentNode]"),
            R"(["0a<I>!b1",null])");
  EXPECT_EQ(eval("const t = root.firstChild; t.after('x'); t.before('y'); t.replaceWith(first);"
                 "root.firstChild.nextSibling === first"),
            "true");
  EXPECT_EQ(eval("document.createElement('p').after('nothing'); 'no parent'"), R"("no parent")");
}

TEST_F(WebDomTest, Events) {
  EXPECT_EQ(eval("var order = []; const root = document.getElementById('root'); const first = "
                 "document.getElementById('first');"
                 "window.addEventListener('ping', () => order.push('window capture'), true);"
                 "document.addEventListener('ping', e => order.push('document ' + e.eventPhase));"
                 "root.addEventListener('ping', e => order.push('root capture ' + e.eventPhase), {capture: true});"
                 "root.addEventListener('ping', e => order.push('root bubble ' + (e.target === first)));"
                 "first.addEventListener('ping', {handleEvent(e) { order.push('first ' + e.eventPhase); }});"
                 "first.onping = e => order.push('first handler');"
                 "const result = first.dispatchEvent(new CustomEvent('ping', {bubbles: true, detail: 1})); "
                 "[order, result]"),
            R"([["window capture","root capture 1","first 2","first handler","root bubble true","document 3"],true])");
  EXPECT_EQ(eval("order = []; first.dispatchEvent(new Event('ping')); order"),
            R"(["window capture","root capture 1","first 2","first handler"])");
  EXPECT_EQ(eval("order = []; root.addEventListener('stop', e => { e.stopPropagation(); order.push(1); }, true);"
                 "first.addEventListener('stop', () => order.push(2)); first.dispatchEvent(new Event('stop'));"
                 "first.addEventListener('now', e => { e.stopImmediatePropagation(); order.push(3); });"
                 "first.addEventListener('now', () => order.push(4)); first.dispatchEvent(new Event('now')); order"),
            "[1,3]");
  EXPECT_EQ(eval("let count = 0; const once = () => ++count; first.addEventListener('once', once, {once: true});"
                 "first.addEventListener('once', once, {once: true}); first.dispatchEvent(new Event('once'));"
                 "first.dispatchEvent(new Event('once')); const removed = () => ++count;"
                 "first.addEventListener('rm', removed); first.removeEventListener('rm', removed);"
                 "first.removeEventListener('none', removed); first.dispatchEvent(new Event('rm')); count"),
            "1");
  EXPECT_EQ(eval("first.addEventListener('cancel', e => e.preventDefault()); "
                 "[first.dispatchEvent(new Event('cancel', {cancelable: true})), "
                 "first.dispatchEvent(new Event('cancel'))]"),
            "[false,true]");
  EXPECT_EQ(eval("const event = new Event('x', {cancelable: true}); event.returnValue = false;"
                 "[event.defaultPrevented, event.returnValue, event.cancelBubble, event.srcElement, event.isTrusted]"),
            "[true,false,false,null,false]");
  EXPECT_EQ(
      eval("event.cancelBubble = true; first.dispatchEvent(event); [event.cancelBubble, event.composedPath()[0] === "
           "first, event.composedPath().length, Event.AT_TARGET]"),
      "[true,true,6,2]");
  EXPECT_EQ(eval("first.onfalse = () => false; const falseEvent = new Event('false', {cancelable: true});"
                 "first.dispatchEvent(falseEvent); falseEvent.defaultPrevented"),
            "true");
  // The event handler content attributes.
  EXPECT_EQ(eval("first.setAttribute('onclick', 'window.clicked = event.type + this.id');"
                 "first.dispatchEvent(new Event('click')); clicked"),
            R"("clickfirst")");
  EXPECT_EQ(eval("first.setAttribute('onbroken', 'syntax error here'); first.dispatchEvent(new Event('broken'));"
                 "'still running'"),
            R"("still running")");
  EXPECT_EQ(errorName("first.dispatchEvent({type: 'x'})"), R"("TypeError")");
  EXPECT_EQ(errorName("new Event()"), R"("TypeError")");
  EXPECT_EQ(eval("first.addEventListener('x', null); new CustomEvent('c').detail"), "null");
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].starts_with("SyntaxError")) << page.uncaughtErrors()[0];
}

TEST_F(WebDomTest, ChangesOfTheDocumentFromCppAreVisible) {
  HtmlElement body = page.document().body();
  body.setAttribute("data-from", "cpp");
  body.appendChild(page.document().createElement("section"));
  EXPECT_EQ(eval("[document.body.dataset.from, document.body.lastChild.tagName]"), R"(["cpp","SECTION"])");
  // And the scripts change the document of the page.
  eval("document.body.setAttribute('data-from', 'js'); 0");
  const WebPage& constPage = page;
  EXPECT_EQ(constPage.document().body().attribute("data-from"), "js");
}

TEST_F(WebDomTest, DocumentWriteOutsideOfAParserScriptIsIgnored) {
  EXPECT_EQ(eval("document.write('<p id=written>'); document.writeln('x'); document.getElementById('written')"),
            "null");
}

}  // namespace
}  // namespace aeronet
