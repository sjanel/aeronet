// The DOM bindings of a page, through WebPage::evaluate(): wrappers, navigation, text nodes, mutations, queries, the
// errors of the DOM, and the helpers of the prelude (events, classList, dataset...).
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/test_server_fixture.hpp"
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

class WebDomTest : public test::PageTest {
 protected:
  WebDomTest() : PageTest(test::PageUrl(gServer, kPage)) {}
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
  EXPECT_EQ(eval("var meta = document.createElement('meta'); meta.setAttribute('content', 'c');"
                 "meta.content = 'updated'; const div = document.getElementById('root'); div.content = 'own';"
                 "const fo = document.getElementById('fo'); const foBefore = fo.content; fo.content = 'svg';"
                 "[meta.content, meta.getAttribute('content'), div.content, Object.hasOwn(div, 'content'), "
                 "foBefore, fo.content]"),
            R"(["updated","updated","own",true,null,"svg"])");
  // The content of a template is read-only: assigning it is ignored, as in the non-strict code of classic scripts.
  EXPECT_EQ(eval("const tpl = document.getElementById('tpl'); const tplContent = tpl.content; tpl.content = 'x';"
                 "[tpl.content === tplContent, Object.hasOwn(tpl, 'content')]"),
            "[true,false]");
  EXPECT_EQ(errorName("Object.getOwnPropertyDescriptor(Element.prototype, 'content').set.call({}, 1)"),
            R"("TypeError")");
  EXPECT_EQ(errorName("meta.content = { toString() { throw new RangeError(); } }"), R"("RangeError")");
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
                 "root.contains(undefined), root.hasChildNodes(), first.firstChild.hasChildNodes()]"),
            "[true,true,false,false,false,true,false]");
  EXPECT_EQ(errorName("root.contains()"), R"("TypeError")");
  EXPECT_EQ(errorName("root.contains({})"), R"("TypeError")");
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
  // The text of a document fragment is that of its descendants, as for an element.
  EXPECT_EQ(eval("[document.textContent, document.firstChild.textContent, "
                 "document.getElementById('tpl').content.textContent]"),
            R"([null,null,"in templatetext"])");
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
  // A fragment brings its elements: two are one too many, none (only comments) can always be inserted.
  EXPECT_EQ(
      errorName("html.remove(); const two = document.createDocumentFragment();"
                "two.append(document.createElement('a'), document.createElement('b')); document.appendChild(two)"),
      R"("HierarchyRequestError")");
  EXPECT_EQ(eval("const comments = document.createDocumentFragment(); comments.append(document.createComment('d'));"
                 "document.appendChild(comments); [...document.childNodes].map(node => node.nodeType)"),
            "[8,10,8]");
  // The doctype goes before the element, even when other nodes separate them.
  EXPECT_EQ(eval("document.removeChild(doctype); document.insertBefore(doctype, document.lastChild) === doctype && "
                 "document.childNodes[1] === doctype"),
            "true");
  EXPECT_EQ(errorName("document.removeChild(doctype); document.insertBefore(html, document.firstChild);"
                      "document.insertBefore(doctype, document.lastChild)"),
            R"("HierarchyRequestError")");
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

// A template cannot be inserted into its own contents, nor into the contents of a template of them (DOM
// "host-including inclusive ancestor"): the tree would contain itself, and its copies and serializations never end.
TEST_F(WebDomTest, TemplateInsertedIntoItsOwnContents) {
  eval(
      "var outer = document.createElement('template'); var inner = document.createElement('template');"
      "var cell = document.createElement('div'); outer.content.appendChild(inner); inner.content.appendChild(cell); 0");
  EXPECT_EQ(errorName("outer.content.appendChild(outer)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("cell.appendChild(outer)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("cell.appendChild(inner)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("cell.insertAdjacentElement('afterend', outer)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("cell.appendChild(outer.content)"), R"("HierarchyRequestError")");
  // Nothing moved: the copy and the serialization end.
  EXPECT_EQ(eval("outer.cloneNode(true).content.firstChild.content.firstChild.nodeName"), R"("DIV")");
  EXPECT_EQ(eval("outer.innerHTML"), R"("<template><div></div></template>")");
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
      () => document.write('a', boom), () => document.writeln(boom), () => document.writeln('a', boom),
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
  // The code of a timer is converted, and its delay, when it is set.
  for (const std::string_view timer : {"setTimeout", "setInterval"}) {
    const std::string name(timer);
    EXPECT_EQ(errorName(name + "({ toString() { throw new RangeError() } })"), R"("RangeError")");
    EXPECT_EQ(errorName(name + "(() => {}, { valueOf() { throw new RangeError() } })"), R"("RangeError")");
  }
}

TEST_F(WebDomTest, PreludeElementHelpers) {
  EXPECT_EQ(eval("const list = document.getElementById('root').classList; list.add('c', 'a'); list.remove('b');"
                 "[list.value, list.length, list.item(0), list.item(5), list.contains('c'), list.toggle('a'), "
                 "list.toggle('z', true), list.toggle('z', true), list.toggle('y', false), list.toggle('c', 1), "
                 "list.toggle('w', 0), list.toggle('w', 1), list.toggle('w', 0), list.replace('z', 'c'), "
                 "list.replace('q', 'r'), [...list], String(list), list.supports('x')]"),
            R"(["a c",2,"a",null,true,false,true,true,false,true,false,true,false,true,false,["c"],"c",true])");
  EXPECT_EQ(errorName("list.add('')"), R"("SyntaxError")");
  EXPECT_EQ(errorName("list.add('a b')"), R"("InvalidCharacterError")");
  EXPECT_EQ(eval("const seen = []; list.forEach((token, i) => seen.push(token + i)); list.value = 'v w';"
                 "[seen, [...list.keys()], [...list.entries()], document.getElementById('root').className]"),
            R"([["c0"],[0,1],[[0,"v"],[1,"w"]],"v w"])");
  EXPECT_EQ(eval("document.getElementById('root').classList = 'x'; document.getElementById('root').className"),
            R"("x")");
  // An empty token set adds no attribute to an element without one, but keeps an existing one.
  EXPECT_EQ(eval("const bare = document.createElement('p'); bare.classList.remove('x'); bare.classList.add();"
                 "const before = bare.hasAttribute('class');"
                 "[before, bare.classList.toggle('y', false), bare.hasAttribute('class'), bare.classList.toggle('y'),"
                 "bare.classList.toggle('y'), bare.outerHTML]"),
            R"([false,false,false,true,false,"<p class=\"\"></p>"])");

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

// The helpers insert their items as one node (DOM "convert nodes into a node"), beside the siblings that are not among
// them, and change nothing when that node cannot be inserted.
TEST_F(WebDomTest, PreludeHelpersFollowTheDomAlgorithms) {
  eval(
      "const list = document.createElement('ul'); let a, b, c;"
      "const reset = () => { list.innerHTML = '<li>a</li><li>b</li><li>c</li>'; [a, b, c] = Array.from(list.children); "
      "};"
      "0");
  EXPECT_EQ(eval("reset(); a.after(b, c); list.textContent"), R"("abc")");
  EXPECT_EQ(eval("reset(); b.before(b, a); list.textContent"), R"("bac")");
  EXPECT_EQ(eval("reset(); a.replaceWith(b, a); list.textContent"), R"("bac")");
  EXPECT_EQ(eval("reset(); a.replaceWith(c, b); list.textContent"), R"("cb")");
  EXPECT_EQ(eval("reset(); b.before(); b.after(); b.replaceWith(b); list.prepend(); list.append(); list.textContent"),
            R"("abc")");
  // An invalid insertion removes nothing.
  EXPECT_EQ(errorName("reset(); b.replaceWith(list)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("list.replaceChildren(list)"), R"("HierarchyRequestError")");
  EXPECT_EQ(errorName("list.replaceChildren(Object.create(Node.prototype))"), R"("TypeError")");
  EXPECT_EQ(errorName("Element.prototype.replaceChildren.call({}, 'x')"), R"("TypeError")");
  EXPECT_EQ(eval("[list.textContent, b.parentNode === list]"), R"(["abc",true])");
  EXPECT_EQ(eval("list.replaceChildren(c, 'd'); list.innerHTML"), R"("<li>c</li>d")");
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
  // Called without receiver, the methods are those of the window.
  EXPECT_EQ(eval("let bare = 0; const onBare = () => ++bare; addEventListener('bare', onBare);"
                 "dispatchEvent(new Event('bare')); removeEventListener('bare', onBare);"
                 "window.dispatchEvent(new Event('bare')); bare"),
            "1");
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

TEST_F(WebDomTest, CloneNode) {
  // A shallow copy has the attributes, not the children; a deep one has both. Each copy is a node of its own.
  EXPECT_EQ(
      eval("const root = document.getElementById('root'); const shallow = root.cloneNode();"
           "const deep = root.cloneNode(true); root.custom = 1;"
           "[shallow.id, shallow.className, shallow.childNodes.length, shallow.parentNode, deep.innerHTML,"
           "deep !== root, deep.custom, deep.firstChild !== root.firstChild, deep instanceof HTMLDivElement]"),
      R"(["root","a b",0,null,"<p id=\"first\">one<!--note--><b>bold</b></p><p id=\"second\" class=\"b\">two</p>",)"
      R"(true,null,true,true])");
  EXPECT_EQ(eval("[root.cloneNode(false).childNodes.length, root.cloneNode(0).id]"), R"([0,"root"])");
  // Changing the copy leaves the original.
  EXPECT_EQ(eval("deep.firstChild.remove(); deep.setAttribute('id', 'copy');"
                 "[root.children.length, root.id, deep.children.length, document.getElementById('copy')]"),
            R"([2,"root",1,null])");
  // The contents of a template are copied by a deep copy only.
  EXPECT_EQ(eval("const tpl = document.getElementById('tpl');"
                 "[tpl.cloneNode().content.childNodes.length, tpl.cloneNode(true).innerHTML,"
                 "tpl.cloneNode(true).content !== tpl.content]"),
            R"([0,"<i>in template</i>text",true])");
  EXPECT_EQ(eval("const nested = document.createElement('template'); nested.innerHTML = '<b><template><u>x</u>"
                 "</template></b>'; nested.cloneNode(true).content.firstChild.firstChild.content.firstChild.outerHTML"),
            R"("<u>x</u>")");
  // Text, comment, fragment and doctype nodes.
  EXPECT_EQ(eval("const p = document.getElementById('first'); const text = p.firstChild.cloneNode();"
                 "const comment = p.childNodes[1].cloneNode(); const fragment = tpl.content.cloneNode(true);"
                 "const doctype = document.firstChild.cloneNode();"
                 "[text.data, text.parentNode, comment.data, fragment.nodeType, fragment.childNodes.length,"
                 "doctype.nodeType, doctype !== document.firstChild, tpl.content.cloneNode().childNodes.length]"),
            R"(["one",null,"note",11,2,10,true,0])");
  EXPECT_EQ(errorName("document.cloneNode()"), R"("NotSupportedError")");
}

TEST_F(WebDomTest, DocumentFragments) {
  EXPECT_EQ(eval("const fragment = document.createDocumentFragment();"
                 "fragment.append('a', document.createElement('i'), document.createElement('b'));"
                 "[fragment instanceof DocumentFragment, fragment.nodeType, fragment.nodeName, fragment.parentNode,"
                 "fragment.childNodes.length, fragment.children.length, fragment.firstElementChild.tagName,"
                 "fragment.querySelector('b').tagName, fragment.querySelectorAll('*').length, fragment.textContent,"
                 "fragment.isConnected, fragment.ownerDocument === document]"),
            R"([true,11,"#document-fragment",null,3,2,"I","B",2,"a",false,true])");
  // Inserting a fragment moves its children.
  EXPECT_EQ(eval("document.body.appendChild(fragment) === fragment"), "true");
  EXPECT_EQ(
      eval("[fragment.childNodes.length, document.body.lastElementChild.tagName, document.body.lastChild.isConnected]"),
      R"([0,"B",true])");
  EXPECT_EQ(eval("fragment.textContent = 'text'; [fragment.childNodes.length, fragment.firstChild.data]"),
            R"([1,"text"])");
  EXPECT_EQ(eval("fragment.replaceChildren(); fragment.prepend('p'); fragment.textContent"), R"("p")");
  EXPECT_EQ(eval("document.getElementById('tpl').content instanceof DocumentFragment"), "true");
}

TEST_F(WebDomTest, MatchesAndClosest) {
  EXPECT_EQ(eval("const b = document.querySelector('b');"
                 "[b.matches('b'), b.matches('#first > b'), b.matches('i, #root b'), b.matches('p'),"
                 "b.webkitMatchesSelector('div b')]"),
            "[true,true,true,false,true]");
  EXPECT_EQ(eval("[b.closest('b') === b, b.closest('p').id, b.closest('.a').id, b.closest('section'),"
                 "b.closest('body').tagName]"),
            R"([true,"first","root",null,"BODY"])");
  // closest() stops at the root of the tree: a detached element has no ancestor.
  EXPECT_EQ(eval("const detached = document.createElement('div'); detached.innerHTML = '<p><i></i></p>';"
                 "[detached.querySelector('i').closest('div') === detached, detached.closest('body')]"),
            "[true,null]");
  EXPECT_EQ(errorName("b.matches('[')"), R"("SyntaxError")");
  EXPECT_EQ(errorName("b.closest({ toString() { throw new RangeError('selector'); } })"), R"("RangeError")");
  EXPECT_EQ(errorName("b.closest('a >')"), R"("SyntaxError")");
  EXPECT_EQ(errorName("b.matches()"), R"("TypeError")");
}

TEST_F(WebDomTest, CreateElementNs) {
  EXPECT_EQ(eval("const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');"
                 "svg.appendChild(document.createElementNS('http://www.w3.org/2000/svg', 'circle'));"
                 "[svg.namespaceURI, svg.localName, svg instanceof SVGElement, svg instanceof HTMLElement,"
                 "svg.firstChild instanceof SVGElement, svg.outerHTML]"),
            R"(["http://www.w3.org/2000/svg","svg",true,false,true,"<svg><circle></circle></svg>"])");
  EXPECT_EQ(eval("const div = document.createElementNS('http://www.w3.org/1999/xhtml', 'div');"
                 "[div.namespaceURI, div instanceof HTMLDivElement, document.body.namespaceURI,"
                 "document.getElementById('fo').namespaceURI]"),
            R"(["http://www.w3.org/1999/xhtml",true,"http://www.w3.org/1999/xhtml","http://www.w3.org/2000/svg"])");
  EXPECT_EQ(eval("const plain = document.createElementNS(null, 'item'); const other = document.createElementNS("
                 "undefined, 'x'); const prefixed = document.createElementNS('urn:x', 'p:item');"
                 "[plain.namespaceURI, other.namespaceURI, plain instanceof HTMLElement, plain instanceof Element,"
                 "prefixed.namespaceURI, prefixed.localName]"),
            R"([null,null,false,true,"urn:x","item"])");
  // A copy keeps the qualified name: its prefix, and the case of the SVG names of the parser.
  EXPECT_EQ(eval("const copy = prefixed.cloneNode(); const fo = document.getElementById('fo').cloneNode(true);"
                 "[prefixed.tagName, copy.tagName, copy.localName, copy.namespaceURI, fo.tagName, fo.localName]"),
            R"(["p:item","p:item","item","urn:x","foreignObject","foreignObject"])");
  EXPECT_EQ(errorName("document.createElementNS('urn:x', '1a')"), R"("InvalidCharacterError")");
  EXPECT_EQ(errorName("document.createElementNS('urn:x', 'a:b:c')"), R"("InvalidCharacterError")");
  EXPECT_EQ(errorName("document.createElementNS('urn:x', '1:a')"), R"("InvalidCharacterError")");
  EXPECT_EQ(errorName("document.createElementNS(null, 'p:item')"), R"("NamespaceError")");
  EXPECT_EQ(errorName("document.createElementNS('urn:x')"), R"("TypeError")");
  EXPECT_EQ(errorName("document.createElementNS({ toString() { throw new RangeError(); } }, 'a')"), R"("RangeError")");
  EXPECT_EQ(errorName("document.createElementNS(null, { toString() { throw new RangeError(); } })"), R"("RangeError")");
}

// The interface of a tag recognizes its elements (instanceof): they all have the prototype of HTMLElement, so that a
// property lookup does not cross another prototype.
TEST_F(WebDomTest, HtmlElementInterfaces) {
  EXPECT_EQ(eval("const names = ['HTMLMediaElement', 'HTMLAnchorElement', 'HTMLAudioElement', 'HTMLBodyElement',"
                 "'HTMLButtonElement', 'HTMLCanvasElement', 'HTMLDivElement', 'HTMLFormElement', 'HTMLHeadElement',"
                 "'HTMLHeadingElement', 'HTMLHtmlElement', 'HTMLIFrameElement', 'HTMLImageElement', 'HTMLInputElement',"
                 "'HTMLLabelElement', 'HTMLLIElement', 'HTMLLinkElement', 'HTMLMetaElement', 'HTMLOListElement',"
                 "'HTMLOptionElement', 'HTMLParagraphElement', 'HTMLPreElement', 'HTMLScriptElement',"
                 "'HTMLSelectElement', 'HTMLSpanElement', 'HTMLStyleElement', 'HTMLTableElement',"
                 "'HTMLTableCellElement', 'HTMLTableRowElement', 'HTMLTableSectionElement', 'HTMLTemplateElement',"
                 "'HTMLTextAreaElement', 'HTMLTitleElement', 'HTMLUListElement', 'HTMLVideoElement'];"
                 "const tags = ['a', 'audio', 'body', 'button', 'canvas', 'div', 'form', 'head', 'h1', 'h6', 'html',"
                 "'iframe', 'img', 'input', 'label', 'li', 'link', 'meta', 'ol', 'option', 'p', 'pre', 'script',"
                 "'select', 'span', 'style', 'table', 'td', 'th', 'tr', 'thead', 'tbody', 'tfoot', 'template',"
                 "'textarea', 'title', 'ul', 'video', 'section', 'my-element'];"
                 "tags.map(tag => names.filter(name => document.createElement(tag) instanceof window[name]).join('+')"
                 "|| '-').join()"),
            R"("HTMLAnchorElement,HTMLMediaElement+HTMLAudioElement,HTMLBodyElement,HTMLButtonElement,)"
            R"(HTMLCanvasElement,HTMLDivElement,HTMLFormElement,HTMLHeadElement,HTMLHeadingElement,HTMLHeadingElement,)"
            R"(HTMLHtmlElement,HTMLIFrameElement,HTMLImageElement,HTMLInputElement,HTMLLabelElement,HTMLLIElement,)"
            R"(HTMLLinkElement,HTMLMetaElement,HTMLOListElement,HTMLOptionElement,HTMLParagraphElement,HTMLPreElement,)"
            R"(HTMLScriptElement,HTMLSelectElement,HTMLSpanElement,HTMLStyleElement,HTMLTableElement,)"
            R"(HTMLTableCellElement,HTMLTableCellElement,HTMLTableRowElement,HTMLTableSectionElement,)"
            R"(HTMLTableSectionElement,HTMLTableSectionElement,HTMLTemplateElement,HTMLTextAreaElement,)"
            R"(HTMLTitleElement,HTMLUListElement,HTMLMediaElement+HTMLVideoElement,-,-")");
  EXPECT_EQ(eval("const div = document.createElement('div');"
                 "[Object.getPrototypeOf(div) === HTMLElement.prototype, div instanceof HTMLElement,"
                 "Object.getPrototypeOf(HTMLDivElement.prototype) === HTMLElement.prototype,"
                 "Object.getPrototypeOf(HTMLAudioElement.prototype) === HTMLMediaElement.prototype,"
                 "HTMLDivElement.prototype.constructor === HTMLDivElement, HTMLDivElement.name,"
                 "HTMLDivElement === window.HTMLDivElement, 'HTMLSpanElement' in window,"
                 "Object.hasOwn(window, 'HTMLDivElement'), 'HTMLNopeElement' in window,"
                 "document.createElementNS('http://www.w3.org/2000/svg', 'a') instanceof HTMLAnchorElement,"
                 "document.body.firstChild instanceof HTMLDivElement, 1 instanceof HTMLDivElement,"
                 "({}) instanceof HTMLDivElement, HTMLDivElement[Symbol.hasInstance]()]"),
            R"([true,true,true,true,true,"HTMLDivElement",true,true,false,false,false,true,false,false,false])");
  // The headings of each level; the nodes other than elements are no HTML elements.
  EXPECT_EQ(eval("[['h2', 'h3', 'h4', 'h5'].every(tag => document.createElement(tag) instanceof HTMLHeadingElement),"
                 "document.createTextNode('p') instanceof HTMLParagraphElement]"),
            "[true,false]");
  EXPECT_EQ(errorName("new HTMLDivElement()"), R"("TypeError")");
  // A script can replace an interface, as any global, before its first lookup too; other names are not interfaces.
  EXPECT_EQ(eval("window.HTMLIFrameElement = 'mine'; HTMLTableElement = 'assigned';"
                 "[HTMLIFrameElement, HTMLTableElement, Object.hasOwn(window, 'HTMLTableElement'), typeof HTMLElements,"
                 "typeof HTMLFooElement, 'HTMLElementX' in window, typeof HTML]"),
            R"(["mine","assigned",true,"undefined","undefined",false,"undefined"])");
  // The text of the scripts, anchors, options and titles; on another element, a property of its own.
  EXPECT_EQ(eval("const script = document.createElement('script'); script.text = 'let x = 1';"
                 "const other = document.createElement('div'); const before = other.text; other.text = 'own';"
                 "[script.text, script.textContent, document.querySelector('title').text, before, other.text,"
                 "other.textContent, Object.hasOwn(other, 'text')]"),
            R"(["let x = 1","let x = 1"," The \n title ",null,"own","",true])");
  EXPECT_EQ(eval("const link = document.createElement('a'); link.text = 'link'; const option = document.createElement("
                 "'option'); option.text = 'choice'; [link.textContent, option.text, option.textContent]"),
            R"(["link","choice","choice"])");
  EXPECT_EQ(errorName("Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'text').set.call({}, 1)"),
            R"("TypeError")");
}

TEST_F(WebDomTest, DocumentPositions) {
  EXPECT_EQ(
      eval("const first = document.getElementById('first'); const second = document.getElementById('second');"
           "const b = first.querySelector('b');"
           "[first.compareDocumentPosition(second), second.compareDocumentPosition(first),"
           "first.compareDocumentPosition(b), b.compareDocumentPosition(first), b.compareDocumentPosition(second),"
           "first.compareDocumentPosition(first)]"),
      "[4,2,20,10,4,0]");
  EXPECT_EQ(eval("const detached = document.createElement('div');"
                 "[detached.compareDocumentPosition(first) & Node.DOCUMENT_POSITION_DISCONNECTED,"
                 "Node.DOCUMENT_POSITION_CONTAINED_BY, first.DOCUMENT_POSITION_FOLLOWING]"),
            "[1,16,4]");
  // Two disconnected nodes: one precedes the other, in an order of the implementation, the same both ways.
  EXPECT_EQ(eval("const other = document.createElement('div');"
                 "[detached.compareDocumentPosition(other), other.compareDocumentPosition(detached)].sort()"),
            "[35,37]");
  // Nodes of the same depth under different parents (the text of each paragraph).
  EXPECT_EQ(eval("[first.firstChild.compareDocumentPosition(second.firstChild),"
                 "second.firstChild.compareDocumentPosition(first.firstChild)]"),
            "[4,2]");
  EXPECT_EQ(eval("[b.getRootNode() === document, detached.getRootNode() === detached, b.isSameNode(b),"
                 "b.isSameNode(first), b.isSameNode(null), b.isSameNode(undefined)]"),
            "[true,true,true,false,false,false]");
  EXPECT_EQ(errorName("b.isSameNode()"), R"("TypeError")");
  EXPECT_EQ(errorName("b.isSameNode({})"), R"("TypeError")");
  EXPECT_EQ(errorName("b.compareDocumentPosition({})"), R"("TypeError")");
}

TEST_F(WebDomTest, NoLayout) {
  EXPECT_EQ(eval("const root = document.getElementById('root'); const rect = root.getBoundingClientRect();"
                 "[rect instanceof DOMRect, rect.width, rect.height, rect.top, JSON.stringify(rect),"
                 "root.getClientRects().length, root.offsetWidth, root.offsetHeight, root.offsetParent,"
                 "root.clientWidth, root.scrollHeight, root.scrollTop]"),
            R"([true,0,0,0,"{\"x\":0,\"y\":0,\"width\":0,\"height\":0,\"top\":0,\"right\":0,\"bottom\":0,\"left\":0}",)"
            R"(0,0,0,null,0,0,0])");
  EXPECT_EQ(eval("root.scrollTop = 50; root.scrollIntoView(); root.focus(); window.scrollTo(0, 100);"
                 "[root.scrollTop, scrollY, innerWidth, innerHeight, devicePixelRatio, screen.width,"
                 "screen.orientation.type]"),
            R"([0,0,1280,720,1,1280,"landscape-primary"])");
  // The orientation never changes: its listeners are accepted, never called.
  EXPECT_EQ(eval("const onChange = () => {}; screen.orientation.addEventListener('change', onChange);"
                 "screen.orientation.removeEventListener('change', onChange); screen.orientation.angle"),
            "0");
  EXPECT_EQ(eval("const r = new DOMRect(1, 2, -3, 4); r.width = 5; const ro = DOMRectReadOnly.fromRect({x: 1, y: 2});"
                 "ro.x = 9; [r.left, r.right, r.width, r.bottom, ro.x, ro.y, ro.width]"),
            "[1,6,5,6,1,2,0]");
}

TEST_F(WebDomTest, ComputedStyleIsTheInlineStyle) {
  EXPECT_EQ(eval("const div = document.createElement('div');"
                 "div.setAttribute('style', 'display: none; Background-Color:red;;bad');"
                 "div.style.color = 'blue'; const style = getComputedStyle(div);"
                 "[style.display, style.getPropertyValue('background-color'), style.backgroundColor, style.color,"
                 "style.position, style instanceof CSSStyleDeclaration]"),
            R"(["none","red","red","blue","",true])");
  // The properties that no declaration sets are '', as for an element that is not rendered; the methods are found.
  EXPECT_EQ(eval("[style.transitionDuration.split(',').length, style['z-index'], 'opacity' in style,"
                 "typeof style.getPropertyValue, style[Symbol.iterator], style.cssText.includes('display: none')]"),
            R"([1,"",false,"function",null,true])");
  EXPECT_EQ(errorName("getComputedStyle(document)"), R"("TypeError")");
}

TEST_F(WebDomTest, MediaQueries) {
  EXPECT_EQ(eval("['(min-width: 768px)', '(max-width: 600px)', '(min-width: 80em)', '(width >= 40rem)',"
                 "'(width < 1280px)', '(height <= 720px)', 'screen and (min-width: 1024px)', 'print',"
                 "'not print', 'only screen', '(orientation: landscape)', '(orientation: portrait)',"
                 "'(prefers-color-scheme: dark)', '(prefers-color-scheme: light)',"
                 "'(prefers-reduced-motion: reduce)', '(hover: hover)', '(pointer: coarse)', '(any-pointer)',"
                 "'(unknown-feature)', 'print, (min-width: 1px)', '', '(min-width: abc)']"
                 ".map(query => matchMedia(query).matches)"),
            "[true,false,true,true,false,true,true,false,true,true,true,false,false,true,false,true,false,true,"
            "false,true,true,false]");
  EXPECT_EQ(eval("const list = matchMedia('(min-width: 1px)'); let changes = 0; list.addListener(() => ++changes);"
                 "list.addEventListener('change', () => ++changes); list.dispatchEvent(new Event('change'));"
                 "[list.media, list instanceof MediaQueryList, list instanceof EventTarget, list.onchange, changes]"),
            R"json(["(min-width: 1px)",true,true,null,2])json");
  EXPECT_EQ(errorName("matchMedia()"), R"("TypeError")");
}

}  // namespace
}  // namespace aeronet
