#include "script-queue.hpp"

#include <lexbor/dom/interfaces/node.h>
#include <lexbor/ns/const.h>
#include <lexbor/tag/const.h>

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "aeronet/html-element.hpp"
#include "aeronet/log.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "html-node-access.hpp"
#include "html-tree-walk.hpp"

namespace aeronet::internal {

namespace {

// The JavaScript MIME type essences (MIME Sniffing): a script of another type is a data block.
constexpr std::string_view kJavaScriptTypes[] = {
    "application/ecmascript", "application/javascript", "application/x-ecmascript", "application/x-javascript",
    "text/ecmascript",        "text/javascript",        "text/javascript1.0",       "text/javascript1.1",
    "text/javascript1.2",     "text/javascript1.3",     "text/javascript1.4",       "text/javascript1.5",
    "text/jscript",           "text/livescript",        "text/x-ecmascript",        "text/x-javascript",
};

constexpr std::string_view kTextPrefix = "text/";

// Whether "text/" followed by the language attribute 'language' is a JavaScript type.
bool IsJavaScriptLanguage(std::string_view language) noexcept {
  return std::ranges::any_of(kJavaScriptTypes, [language](std::string_view type) {
    return type.starts_with(kTextPrefix) && CaseInsensitiveEqual(type.substr(kTextPrefix.size()), language);
  });
}

// Whether a <script> is a classic script, the only kind that runs (HTML "prepare the script element"): modules are not
// supported, the nomodule scripts are their fallback for such browsers.
bool IsClassicScript(const HtmlElement& script) noexcept {
  if (script.hasAttribute("type")) {
    const std::string_view type = TrimOws(script.attribute("type"));
    return type.empty() || std::ranges::any_of(kJavaScriptTypes, [type](std::string_view javaScriptType) {
             return CaseInsensitiveEqual(type, javaScriptType);
           });
  }
  if (script.hasAttribute("language")) {
    const std::string_view language = script.attribute("language");
    return language.empty() || IsJavaScriptLanguage(language);
  }
  return true;
}

bool IsScript(const lxb_dom_node_t* pNode) noexcept {
  return pNode->type == LXB_DOM_NODE_TYPE_ELEMENT && pNode->local_name == LXB_TAG_SCRIPT && pNode->ns == LXB_NS_HTML;
}

}  // namespace

void ScriptQueue::collect(lxb_dom_node_t* pFirst, lxb_dom_node_t* pEnd, std::string_view url) {
  Scripts& blockingScripts = _scripts[static_cast<std::size_t>(Kind::Blocking)];
  // The blocking scripts go after the current one, in document order.
  auto insertPos = static_cast<std::ptrdiff_t>(_next[static_cast<std::size_t>(Kind::Blocking)]);
  const auto visit = [&](lxb_dom_node_t* pNode) {
    if (!IsScript(pNode)) {
      return;
    }
    const HtmlElement script = HtmlNodeAccess::Element(pNode);
    if (!IsClassicScript(script)) {
      log::debug("[js {}] script of type '{}' not run", url, script.attribute("type"));
      return;
    }
    // async and defer only apply to external scripts.
    const bool external = script.hasAttribute("src");
    if (external && script.hasAttribute("async")) {
      _scripts[static_cast<std::size_t>(Kind::Async)].push_back(pNode);
    } else if (external && script.hasAttribute("defer")) {
      _scripts[static_cast<std::size_t>(Kind::Defer)].push_back(pNode);
    } else {
      blockingScripts.insert(blockingScripts.begin() + insertPos++, pNode);
    }
  };
  for (lxb_dom_node_t* pNode = pFirst; pNode != pEnd; pNode = pNode->next) {
    visit(pNode);
    WalkDescendants(pNode, [&](lxb_dom_node_t* pDescendant) {
      visit(pDescendant);
      return true;
    });
  }
}

}  // namespace aeronet::internal
