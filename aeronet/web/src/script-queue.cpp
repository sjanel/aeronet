#include "script-queue.hpp"

#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/ns/const.h>
#include <lexbor/tag/const.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/html-element.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/log.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "html-node-access.hpp"
#include "html-tree-walk.hpp"
#include "js-module.hpp"

namespace aeronet::internal {

namespace {

// The JavaScript MIME type essences (MIME Sniffing): a script of another type is a data block.
constexpr std::string_view kJavaScriptTypes[]{
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

// The type of the script of a <script> (HTML "prepare the script element").
enum class ScriptType : uint8_t {
  None,     // a data block (application/json...), an import map, a classic script with nomodule: it does not run
  Classic,  // a classic script
  Module,   // a module script
};

ScriptType DeclaredType(const HtmlElement& script) noexcept {
  if (script.hasAttribute("type")) {
    const std::string_view type = TrimOws(script.attribute("type"));
    if (type.empty() || IsJavaScriptMimeType(type)) {
      return ScriptType::Classic;
    }
    return CaseInsensitiveEqual(type, "module") ? ScriptType::Module : ScriptType::None;
  }
  if (script.hasAttribute("language")) {
    const std::string_view language = script.attribute("language");
    return language.empty() || IsJavaScriptLanguage(language) ? ScriptType::Classic : ScriptType::None;
  }
  return ScriptType::Classic;
}

// The type of the script of 'script' that runs: the nomodule classic scripts are the fallback of the browsers without
// modules, as the pages are without ModulesEnabled(). 'url', of the document, is for the logs.
ScriptType TypeToRun(const HtmlElement& script, std::string_view url) {
  const ScriptType type = DeclaredType(script);
  if (type == ScriptType::Classic && script.hasAttribute("nomodule") && ModulesEnabled()) {
    log::debug("[js {}] nomodule script not run", url);
    return ScriptType::None;
  }
  if (type == ScriptType::None || (type == ScriptType::Module && !ModulesEnabled())) {
    log::debug("[js {}] script of type '{}' not run", url, script.attribute("type"));
    return ScriptType::None;
  }
  return type;
}

bool IsScript(const lxb_dom_node_t* pNode) noexcept {
  return pNode->type == LXB_DOM_NODE_TYPE_ELEMENT && pNode->local_name == LXB_TAG_SCRIPT && pNode->ns == LXB_NS_HTML;
}

// Whether pScript has a child text node with some text: its "child text content" is not empty.
bool HasChildText(const lxb_dom_node_t* pScript) noexcept {
  for (const lxb_dom_node_t* pChild = pScript->first_child; pChild != nullptr; pChild = pChild->next) {
    // The character data interfaces start with their node.
    if (IsTextNode(pChild) && reinterpret_cast<const lxb_dom_character_data_t*>(pChild)->data.length != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool IsJavaScriptMimeType(std::string_view type) noexcept {
  return std::ranges::any_of(
      kJavaScriptTypes, [type](std::string_view javaScriptType) { return CaseInsensitiveEqual(type, javaScriptType); });
}

void ScriptQueue::elementCreated(lxb_dom_node_t* pElement) {
  if (IsScript(pElement)) {
    _created.push_back(pElement);
  }
}

bool ScriptQueue::isCreated(const lxb_dom_node_t* pNode) const noexcept {
  return IsScript(pNode) && std::ranges::find(_created, pNode) != _created.end();
}

bool ScriptQueue::hasCreatedIn(const lxb_dom_node_t* pFirst, const lxb_dom_node_t* pEnd) const noexcept {
  const lxb_dom_node_t* pParent = pFirst->parent;
  return std::ranges::any_of(_created, [pFirst, pEnd, pParent](const lxb_dom_node_t* pScript) {
    if (pScript == pParent) {
      return true;
    }
    // The ancestor of pScript among the children of pParent, if any, is one of the inserted siblings.
    const lxb_dom_node_t* pChild = pScript;
    while (pChild->parent != pParent) {
      if (pChild->parent == nullptr) {
        return false;
      }
      pChild = pChild->parent;
    }
    for (const lxb_dom_node_t* pNode = pFirst; pNode != pEnd; pNode = pNode->next) {
      if (pNode == pChild) {
        return true;
      }
    }
    return false;
  });
}

ScriptQueue::Start ScriptQueue::start(lxb_dom_node_t* pScript, std::string_view url) {
  const auto pos = std::ranges::find(_created, pScript);
  if (pos == _created.end()) {
    return Start::No;  // already started, or not created by a script
  }
  const HtmlElement script = HtmlNodeAccess::Element(pScript);
  const bool external = script.hasAttribute("src");
  if (!external && !HasChildText(pScript)) {
    return Start::No;
  }
  const ScriptType type = TypeToRun(script, url);
  if (type == ScriptType::None) {
    return Start::No;
  }
  _created.erase(pos);
  if (type == ScriptType::Module) {
    return Start::Module;
  }
  return external ? Start::External : Start::Inline;
}

void ScriptQueue::queueInserted(const InsertedScript& script) {
  if (_nextInserted == _inserted.size()) {
    // All taken: their views are not used any more.
    _inserted.clear();
    _insertedText.clear();
    _nextInserted = 0;
    _insertedTextPos = 0;
  }
  // The capacity of the text first: the entry is the only step left that can throw.
  _insertedText.ensureAvailableCapacityExponential(script.url.size() + script.text.size());
  _inserted.emplace_back(script.pScript, script.url.size(), script.text.size(), script.credentials, script.kind);
  _insertedText.unchecked_append(script.url);
  _insertedText.unchecked_append(script.text);
}

ScriptQueue::InsertedScript ScriptQueue::nextInserted() noexcept {
  if (_nextInserted == _inserted.size()) {
    return {{}, {}, nullptr, RequestCredentials::Include, Inserted::Classic};
  }
  const QueuedScript& queued = _inserted[_nextInserted++];
  const char* pText = _insertedText.data() + _insertedTextPos;
  _insertedTextPos += queued.urlSize + queued.textSize;
  return {
      {pText, queued.urlSize},
      {pText + queued.urlSize, queued.textSize},
      queued.pScript,
      queued.credentials,
      queued.kind,
  };
}

void ScriptQueue::collect(lxb_dom_node_t* pFirst, lxb_dom_node_t* pEnd, std::string_view url) {
  vector<ParserScript>& blockingScripts = _scripts[static_cast<std::size_t>(Kind::Blocking)];
  // The blocking scripts go after the current one, in document order.
  auto insertPos = static_cast<std::ptrdiff_t>(_next[static_cast<std::size_t>(Kind::Blocking)]);
  const auto visit = [&](lxb_dom_node_t* pNode) {
    if (!IsScript(pNode)) {
      return;
    }
    const HtmlElement script = HtmlNodeAccess::Element(pNode);
    const ScriptType type = TypeToRun(script, url);
    if (type == ScriptType::None) {
      return;
    }
    if (type == ScriptType::Module) {
      // Deferred, inline or not, unless async.
      _scripts[static_cast<std::size_t>(script.hasAttribute("async") ? Kind::Async : Kind::Defer)].emplace_back(pNode,
                                                                                                                true);
      return;
    }
    // async and defer only apply to external classic scripts.
    const bool external = script.hasAttribute("src");
    if (external && script.hasAttribute("async")) {
      _scripts[static_cast<std::size_t>(Kind::Async)].emplace_back(pNode, false);
    } else if (external && script.hasAttribute("defer")) {
      _scripts[static_cast<std::size_t>(Kind::Defer)].emplace_back(pNode, false);
    } else {
      blockingScripts.emplace(blockingScripts.begin() + insertPos++, pNode, false);
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
