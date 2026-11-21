#include "js-prelude.hpp"

#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "aeronet/raw-bytes.hpp"
#include "js-engine.hpp"

namespace aeronet::internal {

namespace {

// The first part of the prelude: the DOM helpers, events, window, and the lazy globals of the second part. About 18
// KB, compiled once per WebClient (its bytecode is read back by the next pages): keep it small. 'native' holds
// log(level, text), reportError(error), setUrl(url) (history, same origin only), resolveUrl(url) (against the base URL,
// '' if invalid), and the network and text helpers of InstallNetwork(). Adjacent literals of less than 16 KB each, the
// limit of MSVC for a string literal (64 KB once concatenated).
constexpr std::string_view kPrelude =
    R"js((function (native) {
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

)js"
    R"js(  getters(Document.prototype, {
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


  // The network and text APIs (fetch(), XMLHttpRequest, URL...) are most of the prelude, and unused by many pages:
  // native.loadNetwork(shared) runs their part (NetworkPreludeSource()) on the first use of one of their globals.
  let network = null;
  const networkApi = () => network ??= native.loadNetwork({ getters, Event, EventTarget });
  for (const name of ['AbortController', 'AbortSignal', 'Headers', 'ProgressEvent', 'Request', 'Response',
    'TextDecoder', 'TextEncoder', 'URL', 'URLSearchParams', 'XMLHttpRequest', 'XMLHttpRequestEventTarget',
    'XMLHttpRequestUpload', 'fetch']) {
    const set = value => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true });
    Object.defineProperty(globalThis, name, {
      get() {
        const value = networkApi()[name];
        set(value);
        return value;
      },
      set,
      configurable: true,
    });
  }
  navigator.sendBeacon = (url, data) => networkApi().sendBeacon(url, data);

  return {
    dispatch(target, type, bubbles, cancelable) {
      const event = new Event(type, { bubbles, cancelable });
      event.isTrusted = true;
      return dispatch(target, event);
    },
  };
}))js";

// The second part, run by the first use of one of its globals in a page: the network and text APIs. About 34 KB.
constexpr std::string_view kNetworkPrelude =
    R"js((function (native, shared) {
  'use strict';
  const { getters, Event, EventTarget } = shared;

  // --- Network: Headers, Request, Response, fetch(), AbortController ---
  // native.request(method, url, headers, body, mode, credentials, callback) queues a request: 'headers' are lines
  // "name:value" separated by '\n', 'body' null, a string or a non-empty Uint8Array, 'callback' gets the response
  // record [status, statusText, url, redirected, type, headers (names and values in turn), body (ArrayBuffer)], or
  // null and the reason of a network error. native.requestSync() returns the record at once.
  const kInternal = Symbol('internal');
  // Lone surrogates become U+FFFD, as in the UTF-8 that browsers send (USVString).
  const wellFormed = text => text.isWellFormed() ? text : text.toWellFormed();
  const token = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
  const checkName = name => {
    name = String(name);
    if (!token.test(name)) throw new TypeError(`'${name}' is not a valid HTTP header name`);
    return name.toLowerCase();
  };
  const checkValue = value => {
    value = String(value).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, '');
    if (/[\0\n\r]/.test(value)) throw new TypeError(`'${value}' is not a valid HTTP header value`);
    return value;
  };
  const normalizedMethods = ['DELETE', 'GET', 'HEAD', 'OPTIONS', 'POST', 'PUT'];
  const normalizeMethod = method => {
    method = String(method);
    if (!token.test(method)) throw new TypeError(`'${method}' is not a valid HTTP method`);
    const upper = method.toUpperCase();
    if (upper === 'CONNECT' || upper === 'TRACE' || upper === 'TRACK') {
      throw new TypeError(`'${method}' HTTP method is unsupported`);
    }
    return normalizedMethods.includes(upper) ? upper : method;
  };
  const pairs = (init, add) => {
    if (typeof init[Symbol.iterator] === 'function') {
      for (const pair of init) {
        const entry = [...pair];
        if (entry.length !== 2) throw new TypeError('A pair needs a name and a value');
        add(entry[0], entry[1]);
      }
    } else {
      for (const name of Object.keys(init)) add(name, init[name]);
    }
  };

  let sortedHeaders;
  let newHeaders;
  class Headers {
    #list = [];
    #immutable = false;
    constructor(init) {
      if (init === undefined || init === null) return;
      if (typeof init !== 'object') throw new TypeError("Failed to construct 'Headers': not a record nor a sequence");
      pairs(init, (name, value) => this.append(name, value));
    }
    #mutable() {
      if (this.#immutable) throw new TypeError('Headers are immutable');
    }
    append(name, value) {
      this.#mutable();
      this.#list.push([checkName(name), checkValue(value)]);
    }
    delete(name) {
      this.#mutable();
      name = checkName(name);
      this.#list = this.#list.filter(header => header[0] !== name);
    }
    get(name) {
      name = checkName(name);
      const values = this.#list.filter(header => header[0] === name).map(header => header[1]);
      return values.length === 0 ? null : values.join(', ');
    }
    getSetCookie() { return this.#list.filter(header => header[0] === 'set-cookie').map(header => header[1]); }
    has(name) {
      name = checkName(name);
      return this.#list.some(header => header[0] === name);
    }
    set(name, value) {
      this.#mutable();
      name = checkName(name);
      value = checkValue(value);
      const index = this.#list.findIndex(header => header[0] === name);
      if (index < 0) {
        this.#list.push([name, value]);
      } else {
        this.#list = this.#list.filter((header, i) => i < index || header[0] !== name);
        this.#list.splice(index, 0, [name, value]);
      }
    }
    // Sorted by name, the values of a name combined (but those of Set-Cookie).
    #sorted() {
      const sorted = [];
      for (const name of [...new Set(this.#list.map(header => header[0]))].sort()) {
        if (name === 'set-cookie') {
          for (const value of this.getSetCookie()) sorted.push([name, value]);
        } else {
          sorted.push([name, this.get(name)]);
        }
      }
      return sorted;
    }
    forEach(callback, thisArg) {
      for (const [name, value] of this.#sorted()) callback.call(thisArg, value, name, this);
    }
    entries() { return this.#sorted()[Symbol.iterator](); }
    keys() { return this.#sorted().map(header => header[0])[Symbol.iterator](); }
    values() { return this.#sorted().map(header => header[1])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    static {
      sortedHeaders = headers => headers.#sorted();
      newHeaders = (list, immutable) => {
        const headers = new Headers();
        headers.#list = list;
        headers.#immutable = immutable;
        return headers;
      };
    }
  }
  const headerLines = headers => sortedHeaders(headers).map(([name, value]) => name + ':' + value).join('\n');

  // The source of a body: null, a string, a Uint8Array, an ArrayBuffer (of a response).
  const bodyOf = (body, headers) => {
    if (body === null || body === undefined) return null;
    let type = null;
    if (body instanceof ArrayBuffer) {
      body = new Uint8Array(body.slice(0));
    } else if (ArrayBuffer.isView(body)) {
      body = new Uint8Array(body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength));
    } else if (body instanceof URLSearchParams) {
      body = body.toString();
      type = 'application/x-www-form-urlencoded;charset=UTF-8';
    } else {
      body = wellFormed(String(body));
      type = 'text/plain;charset=UTF-8';
    }
    if (type !== null && !headers.has('content-type')) headers.append('content-type', type);
    return body;
  };
  const copyOf = source => source === null || typeof source === 'string' ? source : source.slice(0);
  // The text of a response of XMLHttpRequest: the encoding of its byte order mark, else of contentType, else UTF-8.
  const decodeBody = (bytes, contentType) => bytes.byteLength === 0 ? '' : native.decode(bytes, contentType);
  // UTF-8 only, a byte order mark included (Encoding "UTF-8 decode without BOM"): null for an invalid sequence when
  // fatal, else U+FFFD.
  const decodeUtf8 = (bytes, fatal) => bytes.byteLength === 0 ? '' : native.decodeUtf8(bytes, fatal);
  const hasUtf8Bom = bytes => bytes.length >= 3 && bytes[0] === 0xEF && bytes[1] === 0xBB && bytes[2] === 0xBF;
  // Encoding "UTF-8 decode": the text of a body (text(), json()), whose byte order mark is dropped.
  const utf8Decode = bytes => decodeUtf8(hasUtf8Bom(bytes) ? bytes.subarray(3) : bytes, false);
  // The body argument of native.request().
  const requestBody = source => source === null || source.length === 0 ? null : source;

  let takeSource;
  let peekSource;
  class Body {
    #source;
    #used = false;
    constructor(source) { this.#source = source; }
    get body() { return null; }
    get bodyUsed() { return this.#used; }
    async text() {
      const source = takeSource(this);
      if (source === null) return '';
      return typeof source === 'string' ? source : utf8Decode(new Uint8Array(source.buffer ?? source,
        source.byteOffset ?? 0, source.byteLength));
    }
    async json() { return JSON.parse(await this.text()); }
    async arrayBuffer() {
      const source = takeSource(this);
      if (source === null) return new ArrayBuffer(0);
      if (typeof source === 'string') return native.encodeUtf8(source).buffer;  // well-formed
      return source instanceof ArrayBuffer ? source : source.buffer.slice(source.byteOffset,
        source.byteOffset + source.byteLength);
    }
    async bytes() { return new Uint8Array(await this.arrayBuffer()); }
    async blob() { throw new TypeError('aeronet: Blob is not supported'); }
    async formData() { throw new TypeError('aeronet: FormData is not supported'); }
    static {
      takeSource = body => {
        if (body.#used) throw new TypeError('The body has already been read');
        if (body.#source !== null) body.#used = true;
        return body.#source;
      };
      peekSource = body => {
        if (body.#used) throw new TypeError('The body has already been read');
        return body.#source;
      };
    }
  }

  const modes = { 'cors': 0, 'no-cors': 1, 'same-origin': 2 };
  const credentialModes = { 'include': 0, 'same-origin': 1, 'omit': 2 };
  const choice = (value, values, what) => {
    value = String(value);
    if (!Object.hasOwn(values, value)) throw new TypeError(`'${value}' is not a valid ${what}`);
    return value;
  };
  let newSignal;

  class Request extends Body {
    #url; #method; #headers; #mode; #credentials; #signal; #redirect; #cache; #keepalive; #integrity;
    constructor(input, init) {
      init ??= {};
      const from = input instanceof Request ? input : null;
      let url = from?.url;
      if (from === null) {
        url = native.resolveUrl(String(input));
        if (url === '') throw new TypeError(`Failed to construct 'Request': Invalid URL '${input}'`);
      }
      const method = init.method === undefined ? from?.method ?? 'GET' : normalizeMethod(init.method);
      const headers = new Headers(init.headers === undefined ? from?.headers : init.headers);
      let body = init.body ?? null;
      if (init.body === undefined && from !== null) body = copyOf(peekSource(from));
      if (body !== null && (method === 'GET' || method === 'HEAD')) {
        throw new TypeError("Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
      }
      const mode = init.mode === undefined ? from?.mode ?? 'cors' : choice(init.mode, modes, 'request mode');
      super(bodyOf(body, headers));
      this.#url = url;
      this.#method = method;
      this.#headers = headers;
      this.#mode = mode;
      this.#credentials = init.credentials === undefined ? from?.credentials ?? 'same-origin'
        : choice(init.credentials, credentialModes, 'credentials mode');
      this.#signal = init.signal ?? from?.signal ?? newSignal();
      this.#redirect = String(init.redirect ?? from?.redirect ?? 'follow');
      this.#cache = String(init.cache ?? from?.cache ?? 'default');
      this.#keepalive = !!(init.keepalive ?? from?.keepalive);
      this.#integrity = String(init.integrity ?? from?.integrity ?? '');
    }
    get url() { return this.#url; }
    get method() { return this.#method; }
    get headers() { return this.#headers; }
    get mode() { return this.#mode; }
    get credentials() { return this.#credentials; }
    get signal() { return this.#signal; }
    get redirect() { return this.#redirect; }
    get cache() { return this.#cache; }
    get keepalive() { return this.#keepalive; }
    get integrity() { return this.#integrity; }
    get referrer() { return 'about:client'; }
    get referrerPolicy() { return ''; }
    get destination() { return ''; }
    get duplex() { return 'half'; }
    clone() { return new Request(this); }
  }

  const nullBodyStatuses = [101, 103, 204, 205, 304];
  let record = null;  // the response record of the Response under construction, null for a script one
  class Response extends Body {
    #status; #statusText; #headers; #url = ''; #redirected = false; #type = 'default';
    constructor(body = null, init) {
      if (record !== null) {
        const [status, statusText, url, redirected, type, list, source] = record;
        record = null;
        super(source);
        const headers = [];
        for (let i = 0; i < list.length; i += 2) headers.push([list[i], list[i + 1]]);
        this.#status = status;
        this.#statusText = statusText;
        this.#headers = newHeaders(headers, true);
        this.#url = url;
        this.#redirected = redirected;
        this.#type = type;
        return;
      }
      init ??= {};
      const status = init.status === undefined ? 200 : Number(init.status);
      if (!Number.isInteger(status) || status < 200 || status > 599) {
        throw new RangeError(`Failed to construct 'Response': The status provided (${init.status}) is outside the ` +
          'range [200, 599].');
      }
      if (body !== null && nullBodyStatuses.includes(status)) {
        throw new TypeError("Failed to construct 'Response': Response with null body status cannot have body");
      }
      const headers = new Headers(init.headers);
      super(bodyOf(body, headers));
      this.#status = status;
      this.#statusText = String(init.statusText ?? '');
      this.#headers = headers;
    }
    get type() { return this.#type; }
    get url() { return this.#url; }
    get redirected() { return this.#redirected; }
    get status() { return this.#status; }
    get ok() { return this.#status >= 200 && this.#status < 300; }
    get statusText() { return this.#statusText; }
    get headers() { return this.#headers; }
    clone() {
      const flat = [];
      for (const [name, value] of sortedHeaders(this.#headers)) flat.push(name, value);
      record = [this.#status, this.#statusText, this.#url, this.#redirected, this.#type, flat,
        copyOf(peekSource(this))];
      return new Response();
    }
    static error() {
      record = [0, '', '', false, 'error', [], null];
      return new Response();
    }
    static json(data, init) {
      const text = JSON.stringify(data);
      if (text === undefined) throw new TypeError("Failed to execute 'json' on 'Response': not serializable");
      const headers = new Headers(init?.headers);
      if (!headers.has('content-type')) headers.set('content-type', 'application/json');
      return new Response(text, { ...init, headers });
    }
    static redirect(url, status = 302) {
      const location = native.resolveUrl(String(url));
      if (location === '') throw new TypeError(`Failed to execute 'redirect' on 'Response': Invalid URL '${url}'`);
      if (![301, 302, 303, 307, 308].includes(status)) {
        throw new RangeError(`Failed to execute 'redirect' on 'Response': Invalid status code ${status}`);
      }
      return new Response(null, { status, headers: { location } });
    }
  }
  const responseOf = responseRecord => {
    record = responseRecord;
    return new Response();
  };

)js"
    R"js(  let abortSignal;
  class AbortSignal extends EventTarget {
    #aborted = false;
    #reason = undefined;
    constructor(key) {
      super();
      if (key !== kInternal) throw new TypeError('Illegal constructor');
    }
    get aborted() { return this.#aborted; }
    get reason() { return this.#reason; }
    throwIfAborted() {
      if (this.#aborted) throw this.#reason;
    }
    static abort(reason) {
      const signal = newSignal();
      abortSignal(signal, reason);
      return signal;
    }
    static timeout(milliseconds) {
      const signal = newSignal();
      setTimeout(() => abortSignal(signal, new DOMException('The operation timed out.', 'TimeoutError')), milliseconds);
      return signal;
    }
    static any(signals) {
      const signal = newSignal();
      for (const source of signals) {
        if (source.aborted) {
          abortSignal(signal, source.reason);
          return signal;
        }
      }
      for (const source of signals) source.addEventListener('abort', () => abortSignal(signal, source.reason));
      return signal;
    }
    static {
      abortSignal = (signal, reason) => {
        if (signal.#aborted) return;
        signal.#aborted = true;
        signal.#reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError')
          : reason;
        signal.dispatchEvent(new Event('abort'));
      };
    }
  }
  newSignal = () => new AbortSignal(kInternal);

  class AbortController {
    #signal = newSignal();
    get signal() { return this.#signal; }
    abort(reason) { abortSignal(this.#signal, reason); }
  }

  const fetch = (input, init) => new Promise((resolve, reject) => {
    const request = new Request(input, init);
    const signal = request.signal;
    signal.throwIfAborted();
    // Removed before the response is given: the request is not sent yet.
    const onAbort = () => {
      native.cancelRequest(id);
      reject(signal.reason);
    };
    const id = native.request(request.method, request.url, headerLines(request.headers),
      requestBody(takeSource(request)), modes[request.mode], credentialModes[request.credentials],
      (response, error) => {
        signal.removeEventListener('abort', onAbort);
        if (response === null) reject(new TypeError(error));
        else resolve(responseOf(response));
      });
    signal.addEventListener('abort', onAbort);
  });

  const sendBeacon = (url, data = null) => {
    fetch(url, { method: 'POST', body: data, credentials: 'include', mode: 'no-cors', keepalive: true })
      .catch(() => {});
    return true;
  };

  // --- XMLHttpRequest ---
  class ProgressEvent extends Event {
    constructor(type, init) {
      super(type, init);
      this.lengthComputable = !!init?.lengthComputable;
      this.loaded = init?.loaded ?? 0;
      this.total = init?.total ?? 0;
    }
  }
  class XMLHttpRequestEventTarget extends EventTarget {}
  class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {}
  const invalidState = message => new DOMException(message, 'InvalidStateError');
  const responseTypes = ['', 'arraybuffer', 'blob', 'document', 'json', 'text'];

  class XMLHttpRequest extends XMLHttpRequestEventTarget {
    #state = 0;
    #method = 'GET';
    #url = '';
    #async = true;
    #headers = new Headers();
    #send = false;
    #id = 0;
    #response = null;  // the response record
    #text = null;
    #responseType = '';
    #mime = null;
    #withCredentials = false;
    #failure = null;  // the token of a network error of send() not reported yet
    #timeout = 0;
    #upload = new XMLHttpRequestUpload();
    get readyState() { return this.#state; }
    get upload() { return this.#upload; }
    get timeout() { return this.#timeout; }
    set timeout(value) { this.#timeout = Number(value) || 0; }
    get withCredentials() { return this.#withCredentials; }
    set withCredentials(value) {
      if (this.#state > 1 || this.#send) throw invalidState("The object's state must be UNSENT or OPENED.");
      this.#withCredentials = !!value;
    }
    get responseType() { return this.#responseType; }
    set responseType(type) {
      if (!responseTypes.includes(type)) return;
      if (this.#state >= 3) throw invalidState("The response type cannot be set if the object's state is LOADING or DONE.");
      this.#responseType = type;
    }
    get status() { return this.#response?.[0] ?? 0; }
    get statusText() { return this.#response?.[1] ?? ''; }
    get responseURL() { return this.#response?.[2] ?? ''; }
    get responseText() {
      if (this.#responseType !== '' && this.#responseType !== 'text') {
        throw invalidState("The value is only accessible if the object's 'responseType' is '' or 'text'.");
      }
      return this.#state < 3 || this.#response === null ? '' : this.#decoded();
    }
    get response() {
      if (this.#responseType === '' || this.#responseType === 'text') return this.responseText;
      if (this.#state !== 4 || this.#response === null) return null;
      if (this.#responseType === 'arraybuffer') return this.#response[6];
      if (this.#responseType !== 'json') return null;
      try {
        return JSON.parse(utf8Decode(new Uint8Array(this.#response[6])));
      } catch {
        return null;
      }
    }
    get responseXML() { return null; }
    #decoded() {
      return this.#text ??= decodeBody(new Uint8Array(this.#response[6]), this.#mime ??
        this.getResponseHeader('content-type') ?? '');
    }
    #fire(type) { this.dispatchEvent(new Event(type)); }
    #progress(type) {
      const total = this.#response?.[6].byteLength ?? 0;
      this.dispatchEvent(new ProgressEvent(type, { lengthComputable: total > 0, loaded: total, total }));
    }
    #cancel() {
      if (this.#id !== 0) native.cancelRequest(this.#id);
      this.#id = 0;
      this.#failure = null;
    }
    open(method, url, async = true) {
      try {
        method = normalizeMethod(method);
      } catch (error) {
        throw new DOMException(error.message, error.message.endsWith('unsupported') ? 'SecurityError' : 'SyntaxError');
      }
      const resolved = native.resolveUrl(String(url));
      if (resolved === '') throw new DOMException(`Invalid URL '${url}'`, 'SyntaxError');
      this.#cancel();
      this.#method = method;
      this.#url = resolved;
      this.#async = !!async;
      this.#headers = new Headers();
      this.#send = false;
      this.#response = null;
      this.#text = null;
      if (this.#state !== 1) {
        this.#state = 1;
        this.#fire('readystatechange');
      }
    }
    setRequestHeader(name, value) {
      if (this.#state !== 1 || this.#send) throw invalidState("The object's state must be OPENED.");
      this.#headers.append(name, value);
    }
    overrideMimeType(mime) {
      if (this.#state >= 3) throw invalidState("The object's state must not be LOADING or DONE.");
      this.#mime = String(mime);
    }
    getResponseHeader(name) {
      if (this.#state < 2 || this.#response === null) return null;
      name = String(name).toLowerCase();
      const list = this.#response[5];
      const values = [];
      for (let i = 0; i < list.length; i += 2) if (list[i] === name) values.push(list[i + 1]);
      return values.length === 0 ? null : values.join(', ');
    }
    getAllResponseHeaders() {
      if (this.#state < 2 || this.#response === null) return '';
      const list = this.#response[5];
      const names = [];
      for (let i = 0; i < list.length; i += 2) names.push(list[i]);
      return [...new Set(names)].sort().map(name => `${name}: ${this.getResponseHeader(name)}\r\n`).join('');
    }
    send(body = null) {
      if (this.#state !== 1 || this.#send) throw invalidState("The object's state must be OPENED.");
      if (this.#method === 'GET' || this.#method === 'HEAD') body = null;
      const source = bodyOf(body, this.#headers);
      this.#send = true;
      const args = [this.#method, this.#url, headerLines(this.#headers), requestBody(source), modes.cors,
        this.#withCredentials ? credentialModes.include : credentialModes['same-origin']];
      if (!this.#async) {
        try {
          this.#response = native.requestSync(...args);
        } catch (error) {
          this.#state = 4;
          this.#send = false;
          this.#fire('readystatechange');
          throw error;
        }
        this.#done();
        return;
      }
      this.#progress('loadstart');
      const callback = (response, error) => {
        this.#id = 0;
        if (response === null) {
          this.#fail('error');
          return;
        }
        this.#response = response;
        this.#state = 2;
        this.#fire('readystatechange');
        if (this.#state !== 2) return;  // aborted or opened again by a handler
        this.#state = 3;
        this.#fire('readystatechange');
        this.#progress('progress');
        if (this.#state === 3) this.#done();
      };
      try {
        this.#id = native.request(...args, callback);
      } catch {
        // A request that cannot be made (not http nor https, WebClientConfig::maxRequests...) is a network error,
        // reported as a task like the others, unless abort() or open() comes first.
        const failure = this.#failure = {};
        setTimeout(() => {
          if (this.#failure === failure) {
            this.#failure = null;
            this.#fail('error');
          }
        });
      }
    }
    #done() {
      this.#state = 4;
      this.#send = false;
      this.#fire('readystatechange');
      this.#progress('load');
      this.#progress('loadend');
    }
    #fail(type) {
      this.#response = null;
      this.#state = 4;
      this.#send = false;
      this.#fire('readystatechange');
      this.#progress(type);
      this.#progress('loadend');
    }
    abort() {
      this.#cancel();
      if ((this.#state === 1 && this.#send) || this.#state === 2 || this.#state === 3) this.#fail('abort');
      if (this.#state === 4) {
        this.#state = 0;
        this.#response = null;
      }
    }
  }
  getters(XMLHttpRequest, { UNSENT: { value: 0 }, OPENED: { value: 1 }, HEADERS_RECEIVED: { value: 2 },
    LOADING: { value: 3 }, DONE: { value: 4 } });
  getters(XMLHttpRequest.prototype, { UNSENT: { value: 0 }, OPENED: { value: 1 }, HEADERS_RECEIVED: { value: 2 },
    LOADING: { value: 3 }, DONE: { value: 4 } });
)js"
    R"js(
  // --- TextEncoder, TextDecoder, URLSearchParams, URL ---
  // native.decode(bytes, contentType) decodes a non-empty Uint8Array (byte order mark, charset of contentType, else
  // UTF-8), native.decodeUtf8(bytes, fatal) as UTF-8 only, native.encodeUtf8(text) returns a Uint8Array,
  // native.parseUrl(input, base) a serialized URL or ''.
  const utf8Size = code => code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
  class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(input = '') { return native.encodeUtf8(wellFormed(String(input))); }
    encodeInto(source, destination) {
      source = wellFormed(String(source));
      const bytes = native.encodeUtf8(source);
      let read = source.length;
      let written = bytes.length;
      if (written > destination.length) {
        read = 0;
        written = 0;
        for (const char of source) {
          const size = utf8Size(char.codePointAt(0));
          if (written + size > destination.length) break;
          read += char.length;
          written += size;
        }
      }
      destination.set(bytes.subarray(0, written));
      return { read, written };
    }
  }

  const utf8Labels = ['unicode-1-1-utf-8', 'unicode11utf8', 'unicode20utf8', 'utf-8', 'utf8', 'x-unicode20utf8'];
  class TextDecoder {
    #fatal;
    #ignoreBOM;
    constructor(label = 'utf-8', options) {
      if (!utf8Labels.includes(String(label).trim().toLowerCase())) {
        throw new RangeError(`aeronet: the encoding '${label}' is not supported, only UTF-8`);
      }
      this.#fatal = !!options?.fatal;
      this.#ignoreBOM = !!options?.ignoreBOM;
    }
    get encoding() { return 'utf-8'; }
    get fatal() { return this.#fatal; }
    get ignoreBOM() { return this.#ignoreBOM; }
    decode(input) {
      if (input === undefined) return '';
      let bytes;
      if (input instanceof ArrayBuffer) bytes = new Uint8Array(input);
      else if (ArrayBuffer.isView(input)) bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
      else throw new TypeError("Failed to execute 'decode' on 'TextDecoder': not an ArrayBuffer nor a view");
      const text = decodeUtf8(!this.#ignoreBOM && hasUtf8Bom(bytes) ? bytes.subarray(3) : bytes, this.#fatal);
      if (text === null) {
        throw new TypeError("Failed to execute 'decode' on 'TextDecoder': The encoded data was not valid.");
      }
      return text;
    }
  }

  const formEncode = value => encodeURIComponent(wellFormed(String(value))).replace(/%20|[!'()~]/g,
    c => c === '%20' ? '+' : '%' + c.charCodeAt(0).toString(16).toUpperCase());
  const isHex = byte => (byte >= 48 && byte <= 57) || (byte >= 65 && byte <= 70) || (byte >= 97 && byte <= 102);
  const formDecode = value => {
    value = value.replace(/\+/g, ' ');
    if (!value.includes('%')) return value;
    const bytes = native.encodeUtf8(value);
    const decoded = new Uint8Array(bytes.length);
    let length = 0;
    for (let i = 0; i < bytes.length; ++i) {
      if (bytes[i] === 37 && i + 2 < bytes.length && isHex(bytes[i + 1]) && isHex(bytes[i + 2])) {
        decoded[length++] = parseInt(String.fromCharCode(bytes[i + 1], bytes[i + 2]), 16);
        i += 2;
      } else {
        decoded[length++] = bytes[i];
      }
    }
    return decodeUtf8(decoded.subarray(0, length), false);
  };

  let linkParams;
  let parseParams;
  let setQuery;
  class URLSearchParams {
    #list = [];
    #url = null;
    constructor(init = '') {
      if (init instanceof URLSearchParams) {
        this.#list = init.#list.map(pair => [...pair]);
      } else if (typeof init === 'object' && init !== null) {
        pairs(init, (name, value) => this.#list.push([String(name), String(value)]));
      } else {
        this.#parse(String(init));
      }
    }
    #parse(query) {
      this.#list = [];
      for (const part of (query.startsWith('?') ? query.slice(1) : query).split('&')) {
        if (part === '') continue;
        const eq = part.indexOf('=');
        this.#list.push(eq < 0 ? [formDecode(part), ''] : [formDecode(part.slice(0, eq)), formDecode(part.slice(eq + 1))]);
      }
    }
    #update() {
      if (this.#url !== null) setQuery(this.#url, this.toString());
    }
    get size() { return this.#list.length; }
    append(name, value) {
      this.#list.push([String(name), String(value)]);
      this.#update();
    }
    delete(name, value) {
      name = String(name);
      this.#list = this.#list.filter(([n, v]) => n !== name || (value !== undefined && v !== String(value)));
      this.#update();
    }
    get(name) {
      name = String(name);
      return this.#list.find(([n]) => n === name)?.[1] ?? null;
    }
    getAll(name) {
      name = String(name);
      return this.#list.filter(([n]) => n === name).map(([, v]) => v);
    }
    has(name, value) {
      name = String(name);
      return this.#list.some(([n, v]) => n === name && (value === undefined || v === String(value)));
    }
    set(name, value) {
      name = String(name);
      value = String(value);
      const index = this.#list.findIndex(([n]) => n === name);
      if (index < 0) {
        this.#list.push([name, value]);
      } else {
        this.#list = this.#list.filter(([n], i) => i < index || n !== name);
        this.#list.splice(index, 0, [name, value]);
      }
      this.#update();
    }
    sort() {
      this.#list.sort(([lhs], [rhs]) => lhs < rhs ? -1 : lhs > rhs ? 1 : 0);
      this.#update();
    }
    forEach(callback, thisArg) {
      for (const [name, value] of this.#list) callback.call(thisArg, value, name, this);
    }
    entries() { return this.#list.map(pair => [...pair])[Symbol.iterator](); }
    keys() { return this.#list.map(([name]) => name)[Symbol.iterator](); }
    values() { return this.#list.map(([, value]) => value)[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return this.#list.map(([name, value]) => formEncode(name) + '=' + formEncode(value)).join('&'); }
    static {
      linkParams = (params, url) => { params.#url = url; };
      parseParams = (params, query) => params.#parse(query);
    }
  }

  // A serialized URL: scheme, authority (user, password, host, port), path, query, fragment.
  const urlSyntax = /^([^:]+:)(\/\/(?:([^:@/]*)(?::([^@/]*))?@)?(\[[^\]]*\]|[^:/?#]*)(?::([0-9]*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
  const parseUrl = (url, base) => native.parseUrl(String(url), base === undefined ? null : String(base));
  class URL {
    #href;
    #parts;
    #params = null;
    constructor(url, base) {
      const href = parseUrl(url, base);
      if (href === '') throw new TypeError(`Failed to construct 'URL': Invalid URL '${url}'`);
      this.#set(href);
    }
    #set(href) {
      const [, protocol, authority, username, password, hostname, port, pathname, search, hash] = urlSyntax.exec(href);
      this.#href = href;
      this.#parts = { protocol, authority: authority !== undefined, username: username ?? '',
        password: password ?? '', hostname: hostname ?? '', port: port ?? '', pathname,
        search: search === undefined || search === '?' ? '' : search, hash: hash === undefined || hash === '#' ? '' : hash };
      if (this.#params !== null) parseParams(this.#params, this.#parts.search);
    }
    // The URL with some components replaced, parsed again: a setter ignores an invalid result.
    #replace(changes) {
      const parts = { ...this.#parts, ...changes };
      const user = parts.username + (parts.password === '' ? '' : ':' + parts.password);
      const authority = parts.authority ? '//' + (user === '' ? '' : user + '@') + parts.hostname +
        (parts.port === '' ? '' : ':' + parts.port) : '';
      const href = native.parseUrl(parts.protocol + authority + parts.pathname + parts.search + parts.hash, null);
      if (href !== '') this.#set(href);
    }
    static canParse(url, base) { return parseUrl(url, base) !== ''; }
    static parse(url, base) {
      const href = parseUrl(url, base);
      return href === '' ? null : new URL(href);
    }
    get href() { return this.#href; }
    set href(value) {
      const href = parseUrl(value);
      if (href === '') throw new TypeError(`Failed to set the 'href' property on 'URL': Invalid URL '${value}'`);
      this.#set(href);
    }
    get origin() {
      const { protocol } = this.#parts;
      return ['http:', 'https:', 'ws:', 'wss:', 'ftp:'].includes(protocol) ? protocol + '//' + this.host : 'null';
    }
    get protocol() { return this.#parts.protocol; }
    set protocol(value) { this.#replace({ protocol: String(value).split(':')[0] + ':' }); }
    get username() { return this.#parts.username; }
    set username(value) { this.#replace({ username: encodeURIComponent(String(value)) }); }
    get password() { return this.#parts.password; }
    set password(value) { this.#replace({ password: encodeURIComponent(String(value)) }); }
    get host() { return this.#parts.hostname + (this.#parts.port === '' ? '' : ':' + this.#parts.port); }
    set host(value) {
      const [, hostname, port] = /^(\[[^\]]*\]|[^:/?#]*)(?::([0-9]*))?/.exec(String(value));
      this.#replace({ hostname, port: port ?? this.#parts.port });
    }
    get hostname() { return this.#parts.hostname; }
    set hostname(value) { this.#replace({ hostname: String(value) }); }
    get port() { return this.#parts.port; }
    set port(value) { this.#replace({ port: /^[0-9]*/.exec(String(value))[0] }); }
    get pathname() { return this.#parts.pathname; }
    set pathname(value) {
      value = String(value);
      this.#replace({ pathname: this.#parts.authority && !value.startsWith('/') ? '/' + value : value });
    }
    get search() { return this.#parts.search; }
    set search(value) {
      value = String(value);
      this.#replace({ search: value === '' || value === '?' ? '' : '?' + value.replace(/^\?/, '') });
    }
    get searchParams() {
      if (this.#params === null) {
        this.#params = new URLSearchParams(this.#parts.search);
        linkParams(this.#params, this);
      }
      return this.#params;
    }
    get hash() { return this.#parts.hash; }
    set hash(value) {
      value = String(value);
      this.#replace({ hash: value === '' || value === '#' ? '' : '#' + value.replace(/^#/, '') });
    }
    toString() { return this.#href; }
    toJSON() { return this.#href; }
    static {
      setQuery = (url, query) => url.#replace({ search: query === '' ? '' : '?' + query });
    }
  }

  return { AbortController, AbortSignal, Headers, ProgressEvent, Request, Response, TextDecoder, TextEncoder, URL,
    URLSearchParams, XMLHttpRequest, XMLHttpRequestEventTarget, XMLHttpRequestUpload, fetch, sendBeacon };
})
)js";

}  // namespace

std::string_view PreludeSource() noexcept { return kPrelude; }

std::string_view NetworkPreludeSource() noexcept { return kNetworkPrelude; }

JSValue PreludeFunction(JSContext* pCtx, std::string_view source, const char* filename, RawBytes& bytecode,
                        const JsLimits& limits) {
  if (bytecode.empty()) {
    // Compiled and written without limits (0 for none), restored before the bytecode is read back under them.
    JSRuntime* pRuntime = JS_GetRuntime(pCtx);
    JS_SetMemoryLimit(pRuntime, 0);
    JS_SetMaxStackSize(pRuntime, 0);
    const JsValue compiled(pCtx, JS_Eval(pCtx, source.data(), source.size(), filename,
                                         JS_EVAL_FLAG_COMPILE_ONLY));  // of type JS_EVAL_TYPE_GLOBAL, 0
    // The JS_WRITE_OBJ_* flags are distinct bits of signed int macros: added rather than or-ed
    // (bugprone-signed-bitwise).
    static constexpr int kFlags = JS_WRITE_OBJ_BYTECODE + JS_WRITE_OBJ_STRIP_SOURCE + JS_WRITE_OBJ_STRIP_DEBUG;
    std::size_t size = 0;
    const std::unique_ptr<uint8_t, JsFreeDeleter> pData(
        JS_IsException(compiled.get()) ? nullptr : JS_WriteObject(pCtx, &size, compiled.get(), kFlags),
        JsFreeDeleter{pCtx});
    JS_SetMemoryLimit(pRuntime, limits.memory);
    JS_SetMaxStackSize(pRuntime, limits.stack);
    if (!pData) {
      return JS_EXCEPTION;
    }
    bytecode.assign(reinterpret_cast<const std::byte*>(pData.get()), size);
  }
  // Read back by the first page too: its functions are those of the next pages.
  const JSValue function =
      JS_ReadObject(pCtx, reinterpret_cast<const uint8_t*>(bytecode.data()), bytecode.size(), JS_READ_OBJ_BYTECODE);
  return JS_IsException(function) ? JS_EXCEPTION : JS_EvalFunction(pCtx, function);
}

}  // namespace aeronet::internal
