#include "js-prelude.hpp"

#include <string_view>

namespace aeronet::internal {

namespace {

// About 16 KB, compiled once per WebClient (its bytecode is read back by the next pages): keep it small. 'native' holds
// log(level, text), reportError(error), setUrl(url) (history, same origin only) and resolveUrl(url) (against the base
// URL, '' if invalid).
constexpr std::string_view kPrelude = R"js((function (native) {
  'use strict';
  const define = (target, properties) => {
    for (const [name, value] of Object.entries(properties)) {
      Object.defineProperty(target, name, { value, writable: true, configurable: true });
    }
  };
  const getters = (target, properties) => {
    for (const [name, descriptor] of Object.entries(properties)) {
      Object.defineProperty(target, name, { ...descriptor, configurable: true });
    }
  };

  // --- Events ---
  const kListeners = Symbol('listeners');
  const kStop = Symbol('stop');
  const kStopNow = Symbol('stopImmediate');
  const kPath = Symbol('path');

  class Event {
    constructor(type, init) {
      if (arguments.length === 0) throw new TypeError('Event: 1 argument required');
      this.type = String(type);
      this.bubbles = !!init?.bubbles;
      this.cancelable = !!init?.cancelable;
      this.composed = !!init?.composed;
      this.defaultPrevented = false;
      this.isTrusted = false;
      this.target = null;
      this.currentTarget = null;
      this.eventPhase = 0;
      this.timeStamp = performance.now();
      this[kStop] = false;
      this[kStopNow] = false;
      this[kPath] = [];
    }
    get srcElement() { return this.target; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(value) { if (!value) this.preventDefault(); }
    get cancelBubble() { return this[kStop]; }
    set cancelBubble(value) { if (value) this[kStop] = true; }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this[kStop] = true; }
    stopImmediatePropagation() { this[kStop] = this[kStopNow] = true; }
    composedPath() { return this[kPath].slice(); }
  }
  getters(Event, { NONE: { value: 0 }, CAPTURING_PHASE: { value: 1 }, AT_TARGET: { value: 2 }, BUBBLING_PHASE: { value: 3 } });

  class CustomEvent extends Event {
    constructor(type, init) {
      super(type, init);
      this.detail = init?.detail ?? null;
    }
  }

  const captureOf = options => typeof options === 'boolean' ? options : !!options?.capture;

  class EventTarget {
    addEventListener(type, callback, options) {
      if (callback == null) return;
      type = String(type);
      const capture = captureOf(options);
      let listeners = this[kListeners];
      if (listeners === undefined) {
        listeners = new Map();
        Object.defineProperty(this, kListeners, { value: listeners });
      }
      let list = listeners.get(type);
      if (list === undefined) listeners.set(type, list = []);
      if (!list.some(listener => listener.callback === callback && listener.capture === capture)) {
        list.push({ callback, capture, once: typeof options === 'object' && !!options?.once, removed: false });
      }
    }
    removeEventListener(type, callback, options) {
      const list = this[kListeners]?.get(String(type));
      const capture = captureOf(options);
      const index = list === undefined ? -1 : list.findIndex(l => l.callback === callback && l.capture === capture);
      if (index >= 0) list.splice(index, 1)[0].removed = true;
    }
    dispatchEvent(event) {
      if (!(event instanceof Event)) throw new TypeError("dispatchEvent: parameter 1 is not of type 'Event'");
      return dispatch(this, event);
    }
  }

  // The event handler content attributes of <body> are those of the window for its events.
  const windowEvents = new Set(['load', 'unload', 'beforeunload', 'pageshow', 'pagehide', 'hashchange', 'popstate',
    'message', 'storage', 'online', 'offline', 'error']);
  function handlerOf(current, type) {
    const handler = current['on' + type];
    if (typeof handler === 'function') return handler;
    const element = current === globalThis ? (windowEvents.has(type) ? document.body : null)
      : current instanceof Element ? current : null;
    const source = element?.getAttribute('on' + type);
    return source == null ? null : new Function('event', source);
  }

  function invoke(current, event, phase) {
    event.currentTarget = current;
    event.eventPhase = phase;
    const list = current[kListeners]?.get(event.type);
    for (const listener of list === undefined ? [] : list.slice()) {
      if (listener.removed || (phase === 1 && !listener.capture) || (phase === 3 && listener.capture)) continue;
      if (listener.once) current.removeEventListener(event.type, listener.callback, listener.capture);
      try {
        if (typeof listener.callback === 'function') listener.callback.call(current, event);
        else listener.callback.handleEvent(event);
      } catch (error) {
        native.reportError(error);
      }
      if (event[kStopNow]) return;
    }
    if (phase === 1) return;
    try {
      const handler = handlerOf(current, event.type);
      if (handler !== null && handler.call(current, event) === false) event.preventDefault();
    } catch (error) {
      native.reportError(error);
    }
  }

  // The window is the parent of the document, but for the load events (DOM "get the parent" of a Document): those of
  // the elements (scripts) never reach it.
  const parentOf = (target, event) => target instanceof Node
    ? target.parentNode ?? (target.nodeType === 9 && event.type !== 'load' ? globalThis : null) : null;

  function dispatch(target, event) {
    const path = [];
    for (let node = parentOf(target, event); node !== null; node = parentOf(node, event)) path.push(node);
    event.target = target;
    event[kPath] = [target, ...path];
    for (let i = path.length - 1; i >= 0 && !event[kStop]; --i) invoke(path[i], event, 1);
    if (!event[kStop]) invoke(target, event, 2);
    for (let i = 0; event.bubbles && i < path.length && !event[kStop]; ++i) invoke(path[i], event, 3);
    event.currentTarget = null;
    event.eventPhase = 0;
    return !event.defaultPrevented;
  }

  class Window extends EventTarget {}
  Object.setPrototypeOf(globalThis, Window.prototype);
  Object.setPrototypeOf(Node.prototype, EventTarget.prototype);
  const nodeTypes = { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4,
    PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10,
    DOCUMENT_FRAGMENT_NODE: 11 };
  for (const [name, value] of Object.entries(nodeTypes)) {
    Object.defineProperty(Node, name, { value });
    Object.defineProperty(Node.prototype, name, { value });
  }

  // --- Element helpers ---
  const whitespace = /[\t\n\f\r ]+/;
  class DOMTokenList {
    #element;
    #attribute;
    constructor(element, attribute) {
      this.#element = element;
      this.#attribute = attribute;
    }
    #tokens() { return [...new Set(this.value.split(whitespace).filter(Boolean))]; }
    #set(tokens) { this.#element.setAttribute(this.#attribute, tokens.join(' ')); }
    #check(token) {
      token = String(token);
      if (token === '') throw new DOMException('The token is empty', 'SyntaxError');
      if (whitespace.test(token)) throw new DOMException('The token contains whitespace', 'InvalidCharacterError');
      return token;
    }
    get length() { return this.#tokens().length; }
    get value() { return this.#element.getAttribute(this.#attribute) ?? ''; }
    set value(value) { this.#element.setAttribute(this.#attribute, String(value)); }
    item(index) { return this.#tokens()[index] ?? null; }
    contains(token) { return this.#tokens().includes(String(token)); }
    add(...tokens) {
      const list = this.#tokens();
      for (const token of tokens.map(t => this.#check(t))) if (!list.includes(token)) list.push(token);
      this.#set(list);
    }
    remove(...tokens) {
      const removed = tokens.map(t => this.#check(t));
      this.#set(this.#tokens().filter(token => !removed.includes(token)));
    }
    toggle(token, force) {
      token = this.#check(token);
      const present = this.contains(token);
      if (present && force !== true) this.remove(token);
      else if (!present && force !== false) this.add(token);
      else return present;
      return !present;
    }
    replace(token, newToken) {
      token = this.#check(token);
      newToken = this.#check(newToken);
      const list = this.#tokens();
      const index = list.indexOf(token);
      if (index < 0) return false;
      list[index] = newToken;
      this.#set([...new Set(list)]);
      return true;
    }
    supports() { return true; }
    forEach(callback, thisArg) { this.#tokens().forEach((token, i) => callback.call(thisArg, token, i, this)); }
    entries() { return this.#tokens().entries(); }
    keys() { return this.#tokens().keys(); }
    values() { return this.#tokens().values(); }
    [Symbol.iterator]() { return this.values(); }
    toString() { return this.value; }
  }

  const toDataAttribute = key => 'data-' + String(key).replace(/[A-Z]/g, c => '-' + c.toLowerCase());
  const toDataKey = name => name.slice(5).replace(/-([a-z])/g, (match, c) => c.toUpperCase());
  const dataValue = (element, key) => typeof key === 'string' ? element.getAttribute(toDataAttribute(key)) : null;
  const newDataset = element => new Proxy({}, {
    get: (target, key) => dataValue(element, key) ?? undefined,
    set: (target, key, value) => { element.setAttribute(toDataAttribute(key), String(value)); return true; },
    deleteProperty: (target, key) => { element.removeAttribute(toDataAttribute(key)); return true; },
    has: (target, key) => dataValue(element, key) !== null,
    ownKeys: () => element.getAttributeNames().filter(name => name.startsWith('data-')).map(toDataKey),
    getOwnPropertyDescriptor: (target, key) => {
      const value = dataValue(element, key);
      return value === null ? undefined : { value, writable: true, enumerable: true, configurable: true };
    },
  });

  // Not reflected in the style attribute, nor computed: scripts can set and read their values.
  class CSSStyleDeclaration {
    getPropertyValue(name) { return this[name] ?? ''; }
    setProperty(name, value) { this[name] = value == null ? '' : String(value); }
    removeProperty(name) {
      const value = this.getPropertyValue(name);
      delete this[name];
      return value;
    }
    get cssText() { return Object.entries(this).map(([name, value]) => `${name}: ${value};`).join(' '); }
    set cssText(value) {}
  }

  const cache = new WeakMap();
  const cached = (element, kind, create) => {
    let values = cache.get(element);
    if (values === undefined) cache.set(element, values = {});
    return values[kind] ??= create(element);
  };
  getters(Element.prototype, {
    classList: { get() { return cached(this, 'classList', e => new DOMTokenList(e, 'class')); },
      set(value) { this.className = value; } },
    dataset: { get() { return cached(this, 'dataset', newDataset); } },
    style: { get() { return cached(this, 'style', () => new CSSStyleDeclaration()); },
      set(value) { this.setAttribute('style', String(value)); } },
    attributes: { get() { return this.getAttributeNames().map(name => ({ name, localName: name, value: this.getAttribute(name) })); } },
  });

  const reflect = attribute => ({ get() { return this.getAttribute(attribute) ?? ''; },
    set(value) { this.setAttribute(attribute, String(value)); } });
  const reflectUrl = attribute => ({ get() {
    const value = this.getAttribute(attribute);
    return value === null ? '' : native.resolveUrl(value) || value;
  }, set(value) { this.setAttribute(attribute, String(value)); } });
  const reflectBoolean = attribute => ({ get() { return this.hasAttribute(attribute); },
    set(value) { if (value) this.setAttribute(attribute, ''); else this.removeAttribute(attribute); } });
  getters(HTMLElement.prototype, {
    src: reflectUrl('src'), href: reflectUrl('href'), action: reflectUrl('action'), name: reflect('name'),
    type: reflect('type'), rel: reflect('rel'), title: reflect('title'),
    lang: reflect('lang'), value: reflect('value'), alt: reflect('alt'), htmlFor: reflect('for'),
    hidden: reflectBoolean('hidden'), disabled: reflectBoolean('disabled'), checked: reflectBoolean('checked'),
    async: reflectBoolean('async'), defer: reflectBoolean('defer'), noModule: reflectBoolean('nomodule'),
  });

  const toNode = item => item instanceof Node ? item : document.createTextNode(String(item));
  const parentNodeMethods = {
    append(...items) { for (const item of items) this.appendChild(toNode(item)); },
    prepend(...items) {
      const first = this.firstChild;
      for (const item of items) this.insertBefore(toNode(item), first);
    },
    replaceChildren(...items) {
      while (this.firstChild !== null) this.removeChild(this.firstChild);
      this.append(...items);
    },
  };
  const childNodeMethods = {
    before(...items) { for (const item of items) this.parentNode?.insertBefore(toNode(item), this); },
    after(...items) {
      const next = this.nextSibling;
      for (const item of items) this.parentNode?.insertBefore(toNode(item), next);
    },
    replaceWith(...items) {
      const parent = this.parentNode;
      const next = this.nextSibling;
      this.remove();
      for (const item of items) parent?.insertBefore(toNode(item), next);
    },
  };
  define(Element.prototype, parentNodeMethods);
  define(Document.prototype, parentNodeMethods);
  define(Element.prototype, childNodeMethods);
  define(CharacterData.prototype, childNodeMethods);

  getters(Document.prototype, {
    title: { get() {
      const title = this.querySelector('title');
      return title === null ? '' : title.textContent.replace(/[\t\n\f\r ]+/g, ' ').trim();
    }, set(value) {
      const title = this.querySelector('title');
      if (title !== null) title.textContent = String(value);
    } },
    location: { get: () => location, set(value) { location.href = value; } },
    URL: { get: () => location.href },
    documentURI: { get: () => location.href },
    defaultView: { get: () => globalThis },
    activeElement: { get() { return this.body; } },
    hidden: { get: () => false },
    visibilityState: { get: () => 'visible' },
    compatMode: { get: () => 'CSS1Compat' },
    contentType: { get: () => 'text/html' },
    referrer: { get: () => '' },
    domain: { get: () => location.hostname },
  });
  define(Document.prototype, { hasFocus: () => true });

  // --- Window ---
  const format = values => values.map(value => {
    if (typeof value === 'string') return value;
    if (value instanceof Error) return value.stack ? `${value}\n${value.stack}` : String(value);
    try {
      return JSON.stringify(value) ?? String(value);
    } catch {
      return String(value);
    }
  }).join(' ');
  const log = level => (...values) => native.log(level, format(values));
  const counts = new Map();
  const starts = new Map();
  const console = {
    debug: log('debug'), log: log('log'), info: log('info'), warn: log('warn'), error: log('error'),
    trace: log('trace'), dir: log('log'), dirxml: log('log'), table: log('log'), group: log('log'),
    groupCollapsed: log('log'), groupEnd() {},
    assert(condition, ...values) { if (!condition) native.log('error', 'Assertion failed: ' + format(values)); },
    count(label = 'default') {
      counts.set(label, (counts.get(label) ?? 0) + 1);
      native.log('log', `${label}: ${counts.get(label)}`);
    },
    countReset(label = 'default') { counts.delete(label); },
    time(label = 'default') { starts.set(label, performance.now()); },
    timeLog(label = 'default') { native.log('log', `${label}: ${performance.now() - (starts.get(label) ?? 0)} ms`); },
    timeEnd(label = 'default') {
      console.timeLog(label);
      starts.delete(label);
    },
  };

  // One entry: the scripts cannot go back, and navigations replace the page.
  class History {
    #state = null;
    #length = 1;
    get length() { return this.#length; }
    get state() { return this.#state; }
    get scrollRestoration() { return 'auto'; }
    set scrollRestoration(value) {}
    pushState(state, unused, url) {
      this.replaceState(state, unused, url);
      ++this.#length;
    }
    replaceState(state, unused, url) {
      if (url != null) native.setUrl(String(url));
      this.#state = state ?? null;
    }
    back() {}
    forward() {}
    go() {}
  }

  define(globalThis, {
    Event, CustomEvent, EventTarget, Window, DOMTokenList, CSSStyleDeclaration, History, console,
    history: new History(),
    requestAnimationFrame: callback => setTimeout(() => callback(performance.now()), 16),
    cancelAnimationFrame: id => clearTimeout(id),
    requestIdleCallback: callback => setTimeout(() => callback({ didTimeout: false, timeRemaining: () => 50 }), 1),
    cancelIdleCallback: id => clearTimeout(id),
  });

  return {
    dispatch(target, type, bubbles, cancelable) {
      const event = new Event(type, { bubbles, cancelable });
      event.isTrusted = true;
      return dispatch(target, event);
    },
  };
}))js";

}  // namespace

std::string_view PreludeSource() noexcept { return kPrelude; }

}  // namespace aeronet::internal
