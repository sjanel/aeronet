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
// The document owns all its nodes: the HtmlElement handles and the views they return are valid as long as it lives, and
// the input bytes can be released after construction. Read-only for now (no DOM mutation).
// Not thread-safe, even through const methods (see HtmlParentNode).
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
  // element for a frameset document (DOM document.body). The parser always creates all three.
  [[nodiscard]] HtmlElement documentElement() const noexcept;
  [[nodiscard]] HtmlElement head() const noexcept;
  [[nodiscard]] HtmlElement body() const noexcept;

  // The JSON object or array literal assigned to 'variable' by an inline <script>, as in
  // "window.__INITIAL_PROPS__ = {...};", the way many server-rendered pages embed the data that their scripts turn into
  // markup. 'variable' must match the assigned expression exactly up to its start, and not be preceded by an identifier
  // character: "window.__INITIAL_PROPS__" or "__INITIAL_PROPS__" both find "window.__INITIAL_PROPS__={...}", "data"
  // does not match "mydata = {...}". The first matching assignment in document order is returned, from its opening to
  // its closing bracket ({...} or [...], strings skipped), or an empty view when there is none or when it is not
  // closed. The view points into the document, to parse with a JSON library (glaze...). Data in a <script
  // type="application/json"> element is simply its textContent().
  [[nodiscard]] std::string_view assignedJson(std::string_view variable) const noexcept;

 private:
  const lxb_encoding_data* _pEncoding{};
};

}  // namespace aeronet
