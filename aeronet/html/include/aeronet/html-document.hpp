#pragma once

#include <string_view>

#include "aeronet/html-element.hpp"

struct lxb_encoding_data;

namespace aeronet {

// A parsed HTML document (WHATWG HTML parser, as browsers parse it), queried with CSS selectors:
//
//   HtmlDocument doc(response.bodyInMemory(), response.headerValueOrEmpty(http::ContentType));
//   for (HtmlElement row : doc.querySelectorAll("table.fees tr")) {
//     std::string text = row.textContent();
//   }
//
// The document owns all its nodes: the HtmlElement handles and the views they return are valid as long as it lives
// (an attribute value until setAttribute() changes it), and the input bytes can be released after construction. Its
// tree can be modified through its elements (see HtmlElement). Not thread-safe, even through const methods (see
// HtmlParentNode).
class HtmlDocument : public HtmlParentNode {
 public:
  // Parses 'html', decoded following the encoding sniffing algorithm of HTML: a byte order mark, then the charset
  // parameter of 'contentType' (the value of a Content-Type header, can be empty), then a <meta> charset declaration in
  // the first 1024 bytes. Without any of them, the document is read as UTF-8 if it is valid UTF-8, else as
  // windows-1252. Invalid UTF-8 sequences of a document declared as UTF-8 are kept as they are; those of other
  // encodings are replaced by U+FFFD. Malformed markup never fails: it is repaired as browsers do. The parsing time
  // and the number of nodes stay linear in the size of 'html': a start tag at a depth of 512 elements (the limit of
  // browsers) first closes the current element, deeper elements becoming siblings, and at most 16 misnested formatting
  // elements (<b>, <font>...) are recreated where they are still open. Throws std::bad_alloc on memory exhaustion.
  explicit HtmlDocument(std::string_view html, std::string_view contentType = {});

  HtmlDocument(const HtmlDocument&) = delete;
  HtmlDocument& operator=(const HtmlDocument&) = delete;

  // The moved-from document can only be destroyed or assigned to. The elements of 'other' stay valid.
  HtmlDocument(HtmlDocument&& other) noexcept;
  HtmlDocument& operator=(HtmlDocument&& other) noexcept;

  ~HtmlDocument();

  // The canonical name of the encoding the document was decoded from: "UTF-8", "windows-1252", "Shift_JIS"...
  [[nodiscard]] std::string_view encoding() const noexcept;

  // The root element (<html>), the <head> element, and the body of the document: its <body> element, or its <frameset>
  // element for a frameset document (DOM document.body). The parser always creates all three, they are invalid once
  // taken out of the tree.
  [[nodiscard]] HtmlElement documentElement() const noexcept;
  [[nodiscard]] HtmlElement head() const noexcept;
  [[nodiscard]] HtmlElement body() const noexcept;

  // Whether 'localName' is a valid element name (DOM), as createElement() needs: one that starts with an ASCII letter
  // and contains no ASCII whitespace, NUL, '/' or '>', or that starts with ':', '_' or a non-ASCII character followed
  // by ASCII letters and digits, '-', '.', ':', '_' and non-ASCII characters only.
  [[nodiscard]] static bool IsValidElementName(std::string_view localName) noexcept;

  // A new HTML element named 'localName' (lower-cased), out of the tree: insert it with HtmlElement::appendChild() or
  // insertAdjacentElement(). Throws std::invalid_argument if 'localName' is not a valid element name (see
  // IsValidElementName()), std::bad_alloc on memory exhaustion.
  [[nodiscard]] HtmlElement createElement(std::string_view localName);

  // The JSON object or array literal assigned to 'variable' by an inline <script>, as in
  // "window.__INITIAL_PROPS__ = {...};", the way many server-rendered pages embed the data that their scripts turn into
  // markup. 'variable' must match the assigned expression exactly up to its start, and not be preceded by an identifier
  // character: "window.__INITIAL_PROPS__" or "__INITIAL_PROPS__" both find "window.__INITIAL_PROPS__={...}", "data"
  // does not match "mydata = {...}". The first matching assignment in document order is returned, from its opening to
  // its closing bracket ({...} or [...], strings skipped), or an empty view when there is none or when it is not
  // closed. The view points into the document, to parse with a JSON library (glaze...). Data in a <script
  // type="application/json"> element is simply its textContent(). Each text node of a script is searched on its own:
  // an assignment split across text nodes by insertAdjacentText() or insertAdjacentHtml() is not found.
  [[nodiscard]] std::string_view assignedJson(std::string_view variable) const noexcept;

 private:
  friend struct internal::HtmlNodeAccess;

  // Parses 'html' as the public constructor, with scripting enabled when 'scripting' is true, as browsers that run
  // scripts do: the contents of a <noscript> element are then a text node instead of markup.
  HtmlDocument(std::string_view html, std::string_view contentType, bool scripting);

  // 'struct': lexbor also has a function of this name, declared before this header in some files.
  const struct lxb_encoding_data* _pEncoding{};
};

}  // namespace aeronet
