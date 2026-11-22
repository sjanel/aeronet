#include "js-prelude.hpp"

#include <quickjs.h>

#include <cassert>
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
  // The globals set up on their first lookup (the lazy parts, the HTML element interfaces) come before EventTarget.
  Object.setPrototypeOf(native.lazyGlobals, EventTarget.prototype);
  Object.setPrototypeOf(Window.prototype, native.lazyGlobals);

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
  define(DocumentFragment.prototype, parentNodeMethods);
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


  return {
    dispatch(target, type, bubbles, cancelable) {
      const event = new Event(type, { bubbles, cancelable });
      event.isTrusted = true;
      return dispatch(target, event);
    },
    // The helpers of the lazy parts (LazyPrelude), set up by Page::lazyPart().
    shared: { getters, Event, EventTarget, CSSStyleDeclaration },
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

// The view part: the geometry, the observers and the media queries of a page without layout. About 7 KB.
constexpr std::string_view kViewPrelude =
    R"js((function (native, shared) {
  'use strict';
  const { getters, EventTarget, CSSStyleDeclaration } = shared;

  let setRect;
  class DOMRectReadOnly {
    #x; #y; #width; #height;
    constructor(x = 0, y = 0, width = 0, height = 0) {
      this.#x = +x;
      this.#y = +y;
      this.#width = +width;
      this.#height = +height;
    }
    static fromRect(rect) { return new this(rect?.x, rect?.y, rect?.width, rect?.height); }
    get x() { return this.#x; }
    get y() { return this.#y; }
    get width() { return this.#width; }
    get height() { return this.#height; }
    get top() { return Math.min(this.#y, this.#y + this.#height); }
    get right() { return Math.max(this.#x, this.#x + this.#width); }
    get bottom() { return Math.max(this.#y, this.#y + this.#height); }
    get left() { return Math.min(this.#x, this.#x + this.#width); }
    toJSON() {
      const { x, y, width, height, top, right, bottom, left } = this;
      return { x, y, width, height, top, right, bottom, left };
    }
    static {
      // The setters of DOMRect.
      setRect = (rect, field, value) => {
        if (field === 'x') rect.#x = +value;
        else if (field === 'y') rect.#y = +value;
        else if (field === 'width') rect.#width = +value;
        else rect.#height = +value;
      };
    }
  }
  class DOMRect extends DOMRectReadOnly {}
  for (const field of ['x', 'y', 'width', 'height']) {
    const get = Object.getOwnPropertyDescriptor(DOMRectReadOnly.prototype, field).get;
    getters(DOMRect.prototype, { [field]: { get, set(value) { setRect(this, field, value); } } });
  }
  const viewport = () => new DOMRectReadOnly(0, 0, innerWidth, innerHeight);

  const checkElement = (target, method) => {
    if (!(target instanceof Element)) throw new TypeError(`${method}: parameter 1 is not of type 'Element'`);
  };
  const checkCallback = (callback, name) => {
    if (typeof callback !== 'function') throw new TypeError(`${name}: parameter 1 is not a function`);
  };

  // The entries of an observer are delivered together by a task: that of a 0 ms timer, the next "rendering".
  class Observer {
    #callback;
    #targets = new Set();
    #records = [];
    constructor(callback, name) {
      checkCallback(callback, name);
      this.#callback = callback;
    }
    observe(target, entry) {
      if (this.#targets.has(target)) return;
      this.#targets.add(target);
      if (this.#records.push(entry) === 1) {
        setTimeout(() => {
          const records = this.takeRecords();
          if (records.length !== 0) this.#callback.call(this, records, this);
        }, 0);
      }
    }
    unobserve(target) {
      this.#targets.delete(target);
      this.#records = this.#records.filter(entry => entry.target !== target);
    }
    disconnect() {
      this.#targets.clear();
      this.#records = [];
    }
    takeRecords() {
      const records = this.#records;
      this.#records = [];
      return records;
    }
  }

  // An entry class with read-only fields from its initializer.
  const entryClass = fields => {
    const kInit = Symbol('init');
    const entry = class {
      constructor(init) { this[kInit] = init; }
    };
    for (const field of fields) getters(entry.prototype, { [field]: { get() { return this[kInit][field]; } } });
    return entry;
  };
  const IntersectionObserverEntry = entryClass(['time', 'target', 'rootBounds', 'boundingClientRect',
    'intersectionRect', 'isIntersecting', 'isVisible', 'intersectionRatio']);
  const ResizeObserverEntry = entryClass(['target', 'contentRect', 'borderBoxSize', 'contentBoxSize',
    'devicePixelContentBoxSize']);

  // Every element intersects the viewport, wholly: lazy content is loaded at once.
  class IntersectionObserver extends Observer {
    #root;
    #rootMargin;
    #thresholds;
    constructor(callback, options) {
      super(callback, 'IntersectionObserver');
      const { root = null, rootMargin = '0px 0px 0px 0px', threshold = 0 } = options ?? {};
      this.#root = root;
      this.#rootMargin = String(rootMargin);
      this.#thresholds = [].concat(threshold).map(Number).sort((a, b) => a - b);
      if (this.#thresholds.some(t => !(t >= 0 && t <= 1))) throw new RangeError('Threshold values must be in [0, 1]');
    }
    get root() { return this.#root; }
    get rootMargin() { return this.#rootMargin; }
    get thresholds() { return this.#thresholds.slice(); }
    observe(target) {
      checkElement(target, 'IntersectionObserver.observe');
      super.observe(target, new IntersectionObserverEntry({ time: performance.now(), target,
        rootBounds: viewport(), boundingClientRect: new DOMRectReadOnly(), intersectionRect: new DOMRectReadOnly(),
        isIntersecting: true, isVisible: true, intersectionRatio: 1 }));
    }
  }

  // Each observed element is reported once, empty.
  class ResizeObserver extends Observer {
    constructor(callback) { super(callback, 'ResizeObserver'); }
    observe(target) {
      checkElement(target, 'ResizeObserver.observe');
      const size = [Object.freeze({ inlineSize: 0, blockSize: 0 })];
      super.observe(target, new ResizeObserverEntry({ target, contentRect: new DOMRectReadOnly(),
        borderBoxSize: size, contentBoxSize: size, devicePixelContentBoxSize: size }));
    }
  }

  // Media queries, evaluated against the viewport of the window: a screen, in landscape, light, with a fine pointer
  // that hovers, without reduced motion. An unknown feature does not match.
  const length = value => {
    const match = /^([0-9]*\.?[0-9]+)(px|em|rem)?$/.exec(value ?? '');
    return match === null ? NaN : Number(match[1]) * (match[2] === 'em' || match[2] === 'rem' ? 16 : 1);
  };
  const compare = (actual, op, value) => {
    switch (op) {
      case '<': return actual < value;
      case '<=': return actual <= value;
      case '>': return actual > value;
      case '>=': return actual >= value;
      default: return actual === value;
    }
  };
  const feature = (name, value) => {
    const dimension = /^(min-|max-)?(width|height|device-width|device-height)$/.exec(name);
    if (dimension !== null) {
      const actual = dimension[2].endsWith('width') ? innerWidth : innerHeight;
      if (value === undefined) return actual > 0;
      return compare(actual, dimension[1] === 'min-' ? '>=' : dimension[1] === 'max-' ? '<=' : '=', length(value));
    }
    switch (name) {
      case 'orientation': return value === 'landscape';
      case 'prefers-color-scheme': return value === 'light';
      case 'prefers-reduced-motion': case 'prefers-reduced-transparency': case 'prefers-contrast':
        return value === 'no-preference';
      case 'hover': case 'any-hover': return value === undefined || value === 'hover';
      case 'pointer': case 'any-pointer': return value === undefined || value === 'fine';
      case 'color': return true;
      case 'display-mode': return value === 'browser';
      case 'scripting': return value === undefined || value === 'enabled';
      default: return false;
    }
  };
  const condition = term => {
    if (term === 'all' || term === 'screen') return true;
    const range = /^\(\s*([a-z-]+)\s*(<=|>=|<|>|=)\s*([^)]+?)\s*\)$/.exec(term);
    if (range !== null) {
      const actual = range[1] === 'width' ? innerWidth : range[1] === 'height' ? innerHeight : NaN;
      return compare(actual, range[2], length(range[3]));
    }
    const plain = /^\(\s*([a-z-]+)\s*(?::\s*([^)]+?)\s*)?\)$/.exec(term);
    return plain !== null && feature(plain[1], plain[2]);
  };
  const matchesMedia = query => query.trim() === '' || query.split(',').some(part => {
    let terms = part.trim().toLowerCase();
    const negated = terms.startsWith('not ');
    terms = terms.replace(/^(not|only)\s+/, '');
    return terms.split(/\s+and\s+/).every(term => condition(term.trim())) !== negated;
  });

  class MediaQueryList extends EventTarget {
    #media;
    constructor(media) {
      super();
      this.#media = media;
      this.onchange = null;
    }
    get media() { return this.#media; }
    get matches() { return matchesMedia(this.#media); }
    addListener(callback) { this.addEventListener('change', callback); }
    removeListener(callback) { this.removeEventListener('change', callback); }
  }

  function matchMedia(query) {
    if (arguments.length === 0) throw new TypeError('matchMedia: 1 argument required');
    return new MediaQueryList(String(query));
  }

  // The declarations of the style attribute, then those set through element.style, by their names as written and
  // camel-cased: no style sheet applies. The other properties are '', as those of an element that is not rendered.
  const camelCase = name => name.replace(/-([a-z])/g, (match, c) => c.toUpperCase());
  const unsetProperties = new Proxy(Object.create(CSSStyleDeclaration.prototype), {
    get: (target, key, receiver) => typeof key === 'string' && !(key in target) ? '' : Reflect.get(target, key, receiver),
  });
  const getComputedStyle = element => {
    checkElement(element, 'getComputedStyle');
    const style = Object.create(unsetProperties);
    const set = (name, value) => {
      name = name.trim();
      if (name === '') return;
      style[name] = value;
      style[camelCase(name)] = value;
    };
    for (const declaration of (element.getAttribute('style') ?? '').split(';')) {
      const colon = declaration.indexOf(':');
      if (colon > 0) set(declaration.slice(0, colon).toLowerCase(), declaration.slice(colon + 1).trim());
    }
    for (const [name, value] of Object.entries(element.style)) set(name, value);
    return style;
  };

  return { DOMRect, DOMRectReadOnly, IntersectionObserver, IntersectionObserverEntry, MediaQueryList, ResizeObserver,
    ResizeObserverEntry, getComputedStyle, matchMedia };
})
)js";

// The Intl part: the Intl API that QuickJS-ng does not have, with the formats of en-US whatever the locale asked for.
// About 22 KB, in adjacent literals of less than 16 KB each (MSVC).
constexpr std::string_view kIntlPrelude =
    R"js((function (native, shared) {
  'use strict';
  const define = (target, properties) => {
    for (const [name, value] of Object.entries(properties)) {
      Object.defineProperty(target, name, { value, writable: true, configurable: true });
    }
  };

  // --- Options and locales ---
  const option = (options, name, values, fallback) => {
    const value = options[name];
    if (value === undefined) return fallback;
    const string = String(value);
    if (values !== undefined && !values.includes(string)) {
      throw new RangeError(`Value ${string} out of range for Intl options property ${name}`);
    }
    return string;
  };
  const digits = (options, name, min, max, fallback) => {
    const value = options[name];
    if (value === undefined) return fallback;
    const number = Number(value);
    if (!(number >= min && number <= max)) throw new RangeError(`${name} value is out of range.`);
    return Math.floor(number);
  };
  const optionsOf = options => options === undefined ? {} : Object(options);

  // A BCP 47 tag, case-normalized: language in lower case, script in title case, region in upper case.
  const canonicalize = tag => {
    if (typeof tag !== 'string' && (typeof tag !== 'object' || tag === null)) {
      throw new TypeError('Language ID should be string or object.');
    }
    tag = String(tag);
    if (!/^[a-z]{2,8}(-[a-z0-9]{1,8})*$/i.test(tag)) throw new RangeError(`Incorrect locale information provided`);
    let extension = false;
    return tag.split('-').map((subtag, index) => {
      extension ||= index !== 0 && subtag.length === 1;
      if (index === 0 || extension) return subtag.toLowerCase();
      if (subtag.length === 4 && /^[a-z]+$/i.test(subtag)) return subtag[0].toUpperCase() + subtag.slice(1).toLowerCase();
      return subtag.length === 2 || /^[0-9]{3}$/.test(subtag) ? subtag.toUpperCase() : subtag.toLowerCase();
    }).join('-');
  };
  const canonicalLocales = locales => {
    if (locales === undefined) return [];
    const list = typeof locales === 'string' || locales instanceof Locale ? [locales] : Array.from(Object(locales));
    return [...new Set(list.map(canonicalize))];
  };
  const baseName = tag => tag.split(/-[a-z0-9]-/i)[0];
  const isEnglish = tag => tag === 'en' || tag.startsWith('en-');
  // The locale of a format: the first English one asked for, else en-US, in which it formats anyway.
  const resolveLocale = locales => baseName(canonicalLocales(locales).find(isEnglish) ?? 'en-US');
  const supportedLocalesOf = locales => canonicalLocales(locales).filter(isEnglish);

  class Locale {
    #tag;
    constructor(tag, options) {
      if (tag === undefined) throw new TypeError('First argument to Intl.Locale constructor can\'t be empty');
      let base = baseName(canonicalize(tag instanceof Locale ? tag.toString() : tag));
      const extensions = canonicalize(String(tag)).slice(base.length);
      const parts = base.split('-');
      const { language = parts[0], script, region } = optionsOf(options);
      const scriptPart = script ?? parts.find((part, i) => i > 0 && part.length === 4);
      const regionPart = region ?? parts.find((part, i) => i > 0 && (part.length === 2 || /^[0-9]{3}$/.test(part)));
      base = [language, scriptPart, regionPart].filter(part => part !== undefined).join('-');
      this.#tag = canonicalize(base + extensions);
    }
    get baseName() { return baseName(this.#tag); }
    get language() { return this.#tag.split('-')[0]; }
    get script() { return this.baseName.split('-').find((part, i) => i > 0 && part.length === 4); }
    get region() {
      return this.baseName.split('-').find((part, i) => i > 0 && (part.length === 2 || /^[0-9]{3}$/.test(part)));
    }
    maximize() { return new Locale(this.#tag); }
    minimize() { return new Locale(this.#tag); }
    toString() { return this.#tag; }
  }

  // --- Numbers ---
  // The decimal digits of a number string in exponent form ("1.5e+21"), or the string itself.
  const plain = str => {
    const match = /^([0-9]+)(?:\.([0-9]+))?e([+-][0-9]+)$/.exec(str);
    if (match === null) return str;
    const all = match[1] + (match[2] ?? '');
    const point = match[1].length + Number(match[3]);
    if (point <= 0) return '0.' + '0'.repeat(-point) + all;
    return point >= all.length ? all + '0'.repeat(point - all.length) : all.slice(0, point) + '.' + all.slice(point);
  };
  // 'str' ("12.3400") with the trailing zeros of its fraction removed down to 'minFraction' digits.
  const trimFraction = (str, minFraction) => {
    const point = str.indexOf('.');
    if (point < 0) return str;
    let end = str.length;
    while (end > point + 1 + minFraction && str[end - 1] === '0') --end;
    return str.slice(0, end === point + 1 ? point : end);
  };
  // 'x' rounded to 'max' significant digits, with at least 'min' of them. Beyond 16, the shortest representation of
  // the double: toPrecision() would show the digits of its binary value (0.1 is 0.1000000000000000055511...).
  const significant = (x, min, max) => {
    let str = x === 0 ? '0' : trimFraction(plain(max > 16 ? String(x) : x.toPrecision(max)), 0);
    const count = x === 0 ? 1 : str.replace('.', '').replace(/^0+/, '').length;
    if (count < min) str += (str.includes('.') ? '' : '.') + '0'.repeat(min - count);
    return str;
  };
  const currencySymbols = { USD: '$', EUR: '€', GBP: '£', JPY: '¥', CNY: 'CN¥', INR: '₹', KRW: '₩', CAD: 'CA$',
    AUD: 'A$', MXN: 'MX$', BRL: 'R$', HKD: 'HK$', NZD: 'NZ$', TWD: 'NT$', ILS: '₪', VND: '₫', PHP: '₱' };
  const narrowSymbols = { CAD: '$', AUD: '$', MXN: '$', HKD: '$', NZD: '$', TWD: '$', CNY: '¥' };
  const currencyDigits = { JPY: 0, KRW: 0, VND: 0, CLP: 0, ISK: 0, UGX: 0, PYG: 0, BHD: 3, KWD: 3, JOD: 3, OMR: 3,
    TND: 3, IQD: 0, LYD: 3 };
  const unitNames = { percent: '%', byte: 'byte', kilobyte: 'kB', megabyte: 'MB', gigabyte: 'GB', terabyte: 'TB',
    bit: 'bit', millimeter: 'mm', centimeter: 'cm', meter: 'm', kilometer: 'km', inch: 'in', foot: 'ft', mile: 'mi',
    gram: 'g', kilogram: 'kg', pound: 'lb', ounce: 'oz', liter: 'L', milliliter: 'mL', millisecond: 'ms',
    second: 'sec', minute: 'min', hour: 'hr', day: 'day', week: 'wk', month: 'mth', year: 'yr', celsius: '°C',
    fahrenheit: '°F', degree: 'deg', 'kilometer-per-hour': 'km/h', 'mile-per-hour': 'mph',
    'meter-per-second': 'm/s' };
  const compactScales = [[1e12, 'T', 'trillion'], [1e9, 'B', 'billion'], [1e6, 'M', 'million'], [1e3, 'K', 'thousand']];

  class NumberFormat {
    #options;
    #format;
    constructor(locales, options) {
      options = optionsOf(options);
      const locale = resolveLocale(locales);
      const style = option(options, 'style', ['decimal', 'percent', 'currency', 'unit'], 'decimal');
      let currency = options.currency;
      if (currency !== undefined) {
        currency = String(currency).toUpperCase();
        if (!/^[A-Z]{3}$/.test(currency)) throw new RangeError(`Invalid currency code : ${currency}`);
      } else if (style === 'currency') {
        throw new TypeError('Currency code is required with currency style.');
      }
      const currencyDisplay = option(options, 'currencyDisplay', ['code', 'symbol', 'narrowSymbol', 'name'], 'symbol');
      const unit = options.unit === undefined ? undefined : String(options.unit);
      if (style === 'unit' && unit === undefined) throw new TypeError('Unit is required with unit style.');
      const unitDisplay = option(options, 'unitDisplay', ['short', 'narrow', 'long'], 'short');
      const notation = option(options, 'notation', ['standard', 'scientific', 'engineering', 'compact'], 'standard');
      const compactDisplay = option(options, 'compactDisplay', ['short', 'long'], 'short');
      const minimumIntegerDigits = digits(options, 'minimumIntegerDigits', 1, 21, 1);
      const currencyFraction = currencyDigits[currency] ?? 2;
      const defaultMin = style === 'currency' && notation !== 'compact' ? currencyFraction : 0;
      const defaultMax = style === 'currency' && notation !== 'compact' ? currencyFraction
        : style === 'percent' || notation === 'compact' ? 0 : 3;
      let minimumFractionDigits = digits(options, 'minimumFractionDigits', 0, 100, undefined);
      let maximumFractionDigits = digits(options, 'maximumFractionDigits', 0, 100, undefined);
      const explicitFraction = minimumFractionDigits !== undefined || maximumFractionDigits !== undefined;
      if (minimumFractionDigits === undefined) {
        minimumFractionDigits = Math.min(defaultMin, maximumFractionDigits ?? defaultMin);
      }
      if (maximumFractionDigits === undefined) maximumFractionDigits = Math.max(defaultMax, minimumFractionDigits);
      if (minimumFractionDigits > maximumFractionDigits) {
        throw new RangeError('maximumFractionDigits value is out of range.');
      }
      let minimumSignificantDigits = digits(options, 'minimumSignificantDigits', 1, 21, undefined);
      let maximumSignificantDigits = digits(options, 'maximumSignificantDigits', 1, 21, undefined);
      if (minimumSignificantDigits !== undefined || maximumSignificantDigits !== undefined) {
        minimumSignificantDigits ??= 1;
        maximumSignificantDigits ??= 21;
        if (minimumSignificantDigits > maximumSignificantDigits) {
          throw new RangeError('maximumSignificantDigits value is out of range.');
        }
      }
      const grouping = options.useGrouping;
      const useGrouping = grouping === undefined ? (notation === 'compact' ? 'min2' : 'auto')
        : grouping === false || grouping === 'false' ? false : grouping === true ? 'always' : String(grouping);
      const signDisplay = option(options, 'signDisplay', ['auto', 'never', 'always', 'exceptZero', 'negative'], 'auto');
      this.#options = { locale, numberingSystem: 'latn', style, currency, currencyDisplay: style === 'currency'
        ? currencyDisplay : undefined, unit, unitDisplay: style === 'unit' ? unitDisplay : undefined,
      minimumIntegerDigits, minimumFractionDigits, maximumFractionDigits, minimumSignificantDigits,
      maximumSignificantDigits, useGrouping, notation, compactDisplay: notation === 'compact' ? compactDisplay
        : undefined, signDisplay, compactRounding: notation === 'compact' && !explicitFraction
        && maximumSignificantDigits === undefined };
    }
    static supportedLocalesOf(locales) { return supportedLocalesOf(locales); }
    get format() { return this.#format ??= value => this.formatToParts(value).map(part => part.value).join(''); }
    formatRange(start, end) { return `${this.format(start)}–${this.format(end)}`; }
    resolvedOptions() {
      const { compactRounding, ...options } = this.#options;
      return Object.fromEntries(Object.entries(options).filter(([, value]) => value !== undefined));
    }
)js"
    R"js(    // The digits of 'x' (not negative, finite) rounded by the options: [integer, fraction] strings.
    #round(x) {
      const o = this.#options;
      let str;
      if (o.compactRounding) {
        str = x < 100 ? significant(x, 1, 2) : plain(x.toFixed(0));
      } else if (o.maximumSignificantDigits !== undefined) {
        str = significant(x, o.minimumSignificantDigits, o.maximumSignificantDigits);
      } else {
        str = trimFraction(plain(x.toFixed(o.maximumFractionDigits)), o.minimumFractionDigits);
      }
      const [integer, fraction = ''] = str.split('.');
      return [integer.padStart(o.minimumIntegerDigits, '0'), fraction];
    }
    formatToParts(value) {
      const o = this.#options;
      let x = typeof value === 'bigint' ? Number(value) : Number(value);
      const negative = x < 0 || Object.is(x, -0);
      const number = [];
      let zero = false;
      if (Number.isNaN(x)) {
        number.push({ type: 'nan', value: 'NaN' });
      } else if (!Number.isFinite(x)) {
        number.push({ type: 'infinity', value: '∞' });
      } else {
        x = Math.abs(x) * (o.style === 'percent' ? 100 : 1);
        let suffix = null;
        let exponent = 0;
        if (o.notation === 'compact') {
          for (const [scale, short, long] of compactScales) {
            if (x >= scale || Number(this.#round(x / (scale / 1000)).join('.')) >= 1000) {
              x /= scale;
              suffix = o.compactDisplay === 'long' ? long : short;
              break;
            }
          }
        } else if (o.notation !== 'standard' && x !== 0) {
          exponent = Math.floor(Math.log10(x));
          if (o.notation === 'engineering') exponent -= ((exponent % 3) + 3) % 3;
          x /= 10 ** exponent;
        }
        const [integer, fraction] = this.#round(x);
        zero = /^0*$/.test(integer + fraction);
        const group = o.useGrouping === 'always' || o.useGrouping === 'auto' ? integer.length > 3
          : o.useGrouping === 'min2' && integer.length > 4;
        if (group) {
          const first = integer.length % 3 || 3;
          number.push({ type: 'integer', value: integer.slice(0, first) });
          for (let i = first; i < integer.length; i += 3) {
            number.push({ type: 'group', value: ',' }, { type: 'integer', value: integer.slice(i, i + 3) });
          }
        } else {
          number.push({ type: 'integer', value: integer });
        }
        if (fraction !== '') number.push({ type: 'decimal', value: '.' }, { type: 'fraction', value: fraction });
        if (suffix !== null) {
          if (o.compactDisplay === 'long') number.push({ type: 'literal', value: ' ' });
          number.push({ type: 'compact', value: suffix });
        }
        if (o.notation === 'scientific' || o.notation === 'engineering') {
          number.push({ type: 'exponentSeparator', value: 'E' });
          if (exponent < 0) number.push({ type: 'exponentMinusSign', value: '-' });
          number.push({ type: 'exponentInteger', value: String(Math.abs(exponent)) });
        }
      }
      let sign = '';
      switch (o.signDisplay) {
        case 'auto': sign = negative ? '-' : ''; break;
        case 'always': sign = negative ? '-' : '+'; break;
        case 'exceptZero': sign = zero ? '' : negative ? '-' : '+'; break;
        case 'negative': sign = negative && !zero ? '-' : ''; break;
        default: break;
      }
      const parts = sign === '' ? [] : [{ type: sign === '-' ? 'minusSign' : 'plusSign', value: sign }];
      if (o.style === 'currency') {
        const symbol = o.currencyDisplay === 'code' || o.currencyDisplay === 'name' ? null
          : (o.currencyDisplay === 'narrowSymbol' ? narrowSymbols[o.currency] : undefined)
            ?? currencySymbols[o.currency] ?? null;
        if (o.currencyDisplay === 'name') {
          parts.push(...number, { type: 'literal', value: ' ' }, { type: 'currency', value: o.currency });
        } else if (symbol === null) {
          parts.push({ type: 'currency', value: o.currency }, { type: 'literal', value: ' ' }, ...number);
        } else {
          parts.push({ type: 'currency', value: symbol }, ...number);
        }
      } else if (o.style === 'percent') {
        parts.push(...number, { type: 'percentSign', value: '%' });
      } else if (o.style === 'unit') {
        const name = o.unitDisplay === 'long' ? o.unit : unitNames[o.unit] ?? o.unit;
        if (name === '%') parts.push(...number, { type: 'unit', value: name });
        else parts.push(...number, { type: 'literal', value: o.unitDisplay === 'narrow' ? '' : ' ' },
          { type: 'unit', value: name });
      } else {
        parts.push(...number);
      }
      return parts;
    }
  }

  // --- Dates: in the local time zone of the process, or in UTC ---
  const weekdays = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
  const months = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October',
    'November', 'December'];
  const offsetText = minutes => {
    const sign = minutes <= 0 ? '+' : '-';
    const total = Math.abs(minutes);
    return `${sign}${String(Math.floor(total / 60)).padStart(2, '0')}:${String(total % 60).padStart(2, '0')}`;
  };
  // The time zone of the process: UTC, else an Etc/GMT zone for a whole hour offset, else its offset ("+05:30").
  const localTimeZone = () => {
    const offset = new Date().getTimezoneOffset();
    if (offset === 0) return 'UTC';
    return offset % 60 === 0 ? `Etc/GMT${offset > 0 ? '+' : '-'}${Math.abs(offset / 60)}` : offsetText(offset);
  };
  const isUtc = zone => /^(etc\/)?(utc|gmt|uct|zulu|universal)$/i.test(zone);
  const dateStyles = {
    full: { weekday: 'long', month: 'long', day: 'numeric', year: 'numeric' },
    long: { month: 'long', day: 'numeric', year: 'numeric' },
    medium: { month: 'short', day: 'numeric', year: 'numeric' },
    short: { month: 'numeric', day: 'numeric', year: '2-digit' },
  };
  const timeStyles = {
    full: { hour: 'numeric', minute: '2-digit', second: '2-digit', timeZoneName: 'long' },
    long: { hour: 'numeric', minute: '2-digit', second: '2-digit', timeZoneName: 'short' },
    medium: { hour: 'numeric', minute: '2-digit', second: '2-digit' },
    short: { hour: 'numeric', minute: '2-digit' },
  };
  const dateFieldNames = ['weekday', 'era', 'year', 'month', 'day', 'dayPeriod', 'hour', 'minute', 'second',
    'fractionalSecondDigits', 'timeZoneName'];
  const textValues = ['narrow', 'short', 'long'];
  const numericValues = ['2-digit', 'numeric'];
  const dateFieldValues = { weekday: textValues, era: textValues, year: numericValues,
    month: [...numericValues, ...textValues], day: numericValues, dayPeriod: textValues, hour: numericValues,
    minute: numericValues, second: numericValues,
    timeZoneName: ['short', 'long', 'shortOffset', 'longOffset', 'shortGeneric', 'longGeneric'] };
  const pad2 = value => String(value).padStart(2, '0');
)js"
    R"js(
  class DateTimeFormat {
    #options;
    #fields;
    #format;
    #utc;
    constructor(locales, options) {
      options = optionsOf(options);
      const locale = resolveLocale(locales);
      const local = localTimeZone();
      let timeZone = options.timeZone === undefined ? local : String(options.timeZone);
      if (isUtc(timeZone)) {
        timeZone = 'UTC';
      } else if (timeZone !== local && !/^[A-Za-z_]+(\/[A-Za-z0-9_+-]+)*$|^[+-][0-9]{2}(:?[0-9]{2})?$/.test(timeZone)) {
        throw new RangeError(`Invalid time zone specified: ${timeZone}`);
      }
      // Another time zone than UTC and that of the process is formatted as UTC: no time zone database.
      this.#utc = timeZone !== local;
      const fields = {};
      for (const name of dateFieldNames) {
        fields[name] = name === 'fractionalSecondDigits' ? digits(options, name, 1, 3, undefined)
          : option(options, name, dateFieldValues[name], undefined);
      }
      const dateStyle = option(options, 'dateStyle', ['full', 'long', 'medium', 'short'], undefined);
      const timeStyle = option(options, 'timeStyle', ['full', 'long', 'medium', 'short'], undefined);
      const explicit = dateFieldNames.some(name => fields[name] !== undefined);
      if ((dateStyle !== undefined || timeStyle !== undefined) && explicit) {
        throw new TypeError(`Can't set option ${dateFieldNames.find(name => fields[name] !== undefined)} when ` +
          `${dateStyle !== undefined ? 'dateStyle' : 'timeStyle'} is used`);
      }
      if (dateStyle !== undefined || timeStyle !== undefined) {
        Object.assign(fields, dateStyles[dateStyle], timeStyles[timeStyle]);
      } else if (!explicit || dateFieldNames.every(name => name === 'timeZoneName' || fields[name] === undefined)) {
        Object.assign(fields, { year: 'numeric', month: 'numeric', day: 'numeric' });
      }
      const hourCycle = option(options, 'hourCycle', ['h11', 'h12', 'h23', 'h24'], undefined);
      const hour12 = options.hour12 !== undefined ? Boolean(options.hour12)
        : hourCycle !== undefined ? hourCycle === 'h11' || hourCycle === 'h12' : true;
      this.#fields = fields;
      this.#options = { locale, calendar: 'gregory', numberingSystem: 'latn', timeZone, dateStyle, timeStyle,
        hourCycle: fields.hour === undefined ? undefined : hourCycle ?? (hour12 ? 'h12' : 'h23'),
        hour12: fields.hour === undefined ? undefined : hour12,
        ...(dateStyle === undefined && timeStyle === undefined ? fields : {}),
        longDate: dateStyle === 'full' || dateStyle === 'long' };
    }
    static supportedLocalesOf(locales) { return supportedLocalesOf(locales); }
    get format() { return this.#format ??= date => this.formatToParts(date).map(part => part.value).join(''); }
    formatRange(start, end) { return `${this.format(start)} – ${this.format(end)}`; }
    resolvedOptions() {
      const { longDate, ...options } = this.#options;
      return Object.fromEntries(Object.entries(options).filter(([, value]) => value !== undefined));
    }
    formatToParts(date) {
      const time = date === undefined ? Date.now() : Number(date instanceof Date ? date.valueOf() : date);
      const d = new Date(time);
      if (Number.isNaN(d.valueOf())) throw new RangeError('Invalid time value');
      const utc = this.#utc;
      const year = utc ? d.getUTCFullYear() : d.getFullYear();
      const month = utc ? d.getUTCMonth() : d.getMonth();
      const day = utc ? d.getUTCDate() : d.getDate();
      const weekday = utc ? d.getUTCDay() : d.getDay();
      const hours = utc ? d.getUTCHours() : d.getHours();
      const minutes = utc ? d.getUTCMinutes() : d.getMinutes();
      const seconds = utc ? d.getUTCSeconds() : d.getSeconds();
      const millis = utc ? d.getUTCMilliseconds() : d.getMilliseconds();
      const f = this.#fields;
      const o = this.#options;
      const parts = [];
      const literal = value => parts.push({ type: 'literal', value });
      const text = (names, index, style) => style === 'long' ? names[index]
        : style === 'short' ? names[index].slice(0, 3) : names[index][0];
      const numeric = (value, style) => style === '2-digit' ? pad2(value % 100) : String(value);
      const yearText = f.year === '2-digit' ? pad2(Math.abs(year) % 100) : String(year);
      const textMonth = f.month !== undefined && f.month !== 'numeric' && f.month !== '2-digit';
      // Date: "Tuesday, January 2, 2018", "Jan 2", "January 2018", or "1/2/2018".
      if (f.weekday !== undefined) {
        parts.push({ type: 'weekday', value: text(weekdays, weekday, f.weekday) });
        if (f.month !== undefined || f.day !== undefined || f.year !== undefined) literal(', ');
      }
      if (textMonth) {
        parts.push({ type: 'month', value: text(months, month, f.month) });
        if (f.day !== undefined) {
          literal(' ');
          parts.push({ type: 'day', value: numeric(day, f.day) });
        }
        if (f.year !== undefined) {
          literal(f.day !== undefined ? ', ' : ' ');
          parts.push({ type: 'year', value: yearText });
        }
      } else {
        const numbers = [];
        if (f.month !== undefined) numbers.push({ type: 'month', value: numeric(month + 1, f.month) });
        if (f.day !== undefined) numbers.push({ type: 'day', value: numeric(day, f.day) });
        if (f.year !== undefined) numbers.push({ type: 'year', value: yearText });
        numbers.forEach((part, i) => {
          if (i !== 0) literal('/');
          parts.push(part);
        });
      }
      if (f.era !== undefined) {
        literal(' ');
        parts.push({ type: 'era', value: year > 0 ? (f.era === 'long' ? 'Anno Domini' : 'AD')
          : (f.era === 'long' ? 'Before Christ' : 'BC') });
      }
      // Time: "11:40:40 PM", "23:40", with the time zone.
      const hasTime = f.hour !== undefined || f.minute !== undefined || f.second !== undefined;
      if (hasTime && parts.length !== 0) literal(o.longDate ? ' at ' : ', ');
      const timeParts = [];
      if (f.hour !== undefined) {
        const h12 = o.hourCycle === 'h11' ? hours % 12 : hours % 12 || 12;
        const h24 = o.hourCycle === 'h24' ? hours || 24 : hours;
        timeParts.push({ type: 'hour', value: o.hour12 ? numeric(h12, f.hour) : pad2(h24) });
      }
      if (f.minute !== undefined) {
        timeParts.push({ type: 'minute', value: f.hour !== undefined ? pad2(minutes) : numeric(minutes, f.minute) });
      }
      if (f.second !== undefined) {
        timeParts.push({ type: 'second', value: timeParts.length !== 0 ? pad2(seconds) : numeric(seconds, f.second) });
      }
      timeParts.forEach((part, i) => {
        if (i !== 0) literal(':');
        parts.push(part);
      });
      if (f.fractionalSecondDigits !== undefined) {
        if (timeParts.length !== 0) literal('.');
        parts.push({ type: 'fractionalSecond', value: String(millis).padStart(3, '0').slice(0, f.fractionalSecondDigits) });
      }
      if (f.hour !== undefined && o.hour12) {
        literal(' ');
        parts.push({ type: 'dayPeriod', value: hours < 12 ? 'AM' : 'PM' });
      }
      if (f.timeZoneName !== undefined) {
        const offset = utc ? 0 : d.getTimezoneOffset();
        const long = f.timeZoneName.startsWith('long');
        let name = offset === 0 ? (long ? 'Coordinated Universal Time' : 'UTC') : 'GMT' + offsetText(offset);
        if (offset !== 0 && !long) name = name.replace(/:00$/, '').replace(/([+-])0([0-9])/, '$1$2');
        if (parts.length !== 0) literal(' ');
        parts.push({ type: 'timeZoneName', value: name });
      }
      return parts;
    }
  }
)js"
    R"js(
  // --- Plural rules, collation, relative times, lists: English ---
  class PluralRules {
    #options;
    #number;
    constructor(locales, options) {
      options = optionsOf(options);
      const type = option(options, 'type', ['cardinal', 'ordinal'], 'cardinal');
      this.#number = new NumberFormat(locales, { ...options, useGrouping: false, style: 'decimal' });
      const { locale, minimumIntegerDigits, minimumFractionDigits, maximumFractionDigits } =
        this.#number.resolvedOptions();
      this.#options = { locale, type, minimumIntegerDigits, minimumFractionDigits, maximumFractionDigits,
        pluralCategories: type === 'ordinal' ? ['few', 'one', 'two', 'other'] : ['one', 'other'] };
    }
    static supportedLocalesOf(locales) { return supportedLocalesOf(locales); }
    select(value) {
      const n = Math.abs(Number(value));
      if (this.#options.type === 'ordinal') {
        if (!Number.isInteger(n)) return 'other';
        const units = n % 10;
        const tens = n % 100;
        if (units === 1 && tens !== 11) return 'one';
        if (units === 2 && tens !== 12) return 'two';
        return units === 3 && tens !== 13 ? 'few' : 'other';
      }
      // "one" for 1 without visible fraction digits ("1.0" is plural).
      return this.#number.format(n) === '1' ? 'one' : 'other';
    }
    selectRange(start, end) { return this.select(end); }
    resolvedOptions() { return { ...this.#options, pluralCategories: this.#options.pluralCategories.slice() }; }
  }

  const stripMarks = str => str.normalize('NFD').replace(/[̀-ͯ]/g, '');
  // Compares by code units, the runs of digits by value with 'numeric'.
  const compareKeys = (a, b, numeric) => {
    if (numeric) {
      const left = a.split(/([0-9]+)/);
      const right = b.split(/([0-9]+)/);
      for (let i = 0; i < left.length && i < right.length; ++i) {
        if (left[i] === right[i]) continue;
        if (i % 2 === 1) {
          const x = left[i].replace(/^0+(?=.)/, '');
          const y = right[i].replace(/^0+(?=.)/, '');
          if (x.length !== y.length) return x.length < y.length ? -1 : 1;
        }
        return left[i] < right[i] ? -1 : 1;
      }
      return Math.sign(left.length - right.length);
    }
    return a < b ? -1 : a > b ? 1 : 0;
  };
  // The first difference of case between 'a' and 'b' (the same letters): lower case first, unless 'upperFirst'.
  const compareCase = (a, b, upperFirst) => {
    for (let i = 0; i < a.length && i < b.length; ++i) {
      if (a[i] !== b[i] && a[i].toLowerCase() === b[i].toLowerCase()) {
        return (a[i] === a[i].toLowerCase()) !== upperFirst ? -1 : 1;
      }
    }
    return 0;
  };
  class Collator {
    #options;
    #compare;
    constructor(locales, options) {
      options = optionsOf(options);
      const usage = option(options, 'usage', ['sort', 'search'], 'sort');
      const sensitivity = option(options, 'sensitivity', ['base', 'accent', 'case', 'variant'], 'variant');
      const ignorePunctuation = Boolean(options.ignorePunctuation);
      const numeric = Boolean(options.numeric);
      const caseFirst = option(options, 'caseFirst', ['upper', 'lower', 'false'], 'false');
      this.#options = { locale: resolveLocale(locales), usage, sensitivity, ignorePunctuation, collation: 'default',
        numeric, caseFirst };
    }
    static supportedLocalesOf(locales) { return supportedLocalesOf(locales); }
    // Letters without marks nor case first (the primary strength of a collation), then marks, then case.
    get compare() {
      return this.#compare ??= (first, second) => {
        const o = this.#options;
        let a = String(first);
        let b = String(second);
        if (o.ignorePunctuation) {
          a = a.replace(/[\s\p{P}]/gu, '');
          b = b.replace(/[\s\p{P}]/gu, '');
        }
        const primary = compareKeys(stripMarks(a).toLowerCase(), stripMarks(b).toLowerCase(), o.numeric);
        if (primary !== 0 || o.sensitivity === 'base') return primary;
        if (o.sensitivity !== 'case') {
          const marks = compareKeys(a.normalize('NFD').toLowerCase(), b.normalize('NFD').toLowerCase(), o.numeric);
          if (marks !== 0 || o.sensitivity === 'accent') return marks;
        }
        const cases = compareCase(stripMarks(a), stripMarks(b), o.caseFirst === 'upper');
        if (cases !== 0 || o.sensitivity === 'case') return cases;
        return compareKeys(a, b, false);
      };
    }
    resolvedOptions() { return { ...this.#options }; }
  }

  const relativeUnits = ['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'];
  const shortUnits = { second: 'sec.', minute: 'min.', hour: 'hr.', week: 'wk.', month: 'mo.', year: 'yr.' };
  const autoPhrases = { second: { 0: 'now' }, minute: { 0: 'this minute' }, hour: { 0: 'this hour' },
    day: { '-1': 'yesterday', 0: 'today', 1: 'tomorrow' } };
  class RelativeTimeFormat {
    #options;
    #number;
    constructor(locales, options) {
      options = optionsOf(options);
      const style = option(options, 'style', ['long', 'short', 'narrow'], 'long');
      const numeric = option(options, 'numeric', ['always', 'auto'], 'always');
      this.#number = new NumberFormat(locales);
      this.#options = { locale: this.#number.resolvedOptions().locale, style, numeric, numberingSystem: 'latn' };
    }
    static supportedLocalesOf(locales) { return supportedLocalesOf(locales); }
    format(value, unit) { return this.formatToParts(value, unit).map(part => part.value).join(''); }
    formatToParts(value, unit) {
      value = Number(value);
      if (!Number.isFinite(value)) throw new RangeError('Invalid value');
      unit = String(unit);
      const singular = unit.endsWith('s') ? unit.slice(0, -1) : unit;
      if (!relativeUnits.includes(singular)) throw new RangeError(`Invalid unit argument for format() '${unit}'`);
      const zero = value === 0;
      if (this.#options.numeric === 'auto') {
        const phrase = autoPhrases[singular]?.[zero ? 0 : value]
          ?? (singular !== 'second' && singular !== 'minute' && singular !== 'hour' && singular !== 'day'
            && (zero || value === 1 || value === -1)
            ? `${zero ? 'this' : value === 1 ? 'next' : 'last'} ${singular}` : undefined);
        if (phrase !== undefined) return [{ type: 'literal', value: phrase }];
      }
      const past = value < 0 || Object.is(value, -0);
      const amount = Math.abs(value);
      const plural = amount === 1 ? '' : 's';
      const name = this.#options.style === 'long' ? singular + plural
        : shortUnits[singular] ?? (singular === 'quarter' ? `qtr${plural}.` : singular + plural);
      const number = this.#number.formatToParts(amount).map(part => ({ ...part, unit: singular }));
      return past ? [...number, { type: 'literal', value: ` ${name} ago` }]
        : [{ type: 'literal', value: 'in ' }, ...number, { type: 'literal', value: ` ${name}` }];
    }
    resolvedOptions() { return { ...this.#options }; }
  }

  class ListFormat {
    #options;
    constructor(locales, options) {
      options = optionsOf(options);
      const type = option(options, 'type', ['conjunction', 'disjunction', 'unit'], 'conjunction');
      const style = option(options, 'style', ['long', 'short', 'narrow'], 'long');
      this.#options = { locale: resolveLocale(locales), type, style };
    }
    static supportedLocalesOf(locales) { return supportedLocalesOf(locales); }
    format(list) { return this.formatToParts(list).map(part => part.value).join(''); }
    formatToParts(list) {
      const items = [];
      for (const item of list ?? []) {
        if (typeof item !== 'string') throw new TypeError(`Iterable yielded ${String(item)} which is not a string`);
        items.push(item);
      }
      const { type, style } = this.#options;
      const word = type === 'disjunction' ? 'or' : type === 'conjunction' && style !== 'narrow'
        ? (style === 'long' ? 'and' : '&') : null;
      const separator = type === 'unit' && style === 'narrow' ? ' ' : ', ';
      const parts = [];
      items.forEach((item, i) => {
        if (i !== 0) {
          const last = i === items.length - 1;
          parts.push({ type: 'literal', value: last && word !== null
            ? (items.length === 2 ? ` ${word} ` : `, ${word} `) : separator });
        }
        parts.push({ type: 'element', value: item });
      });
      return parts;
    }
    resolvedOptions() { return { ...this.#options }; }
  }

  // NumberFormat, DateTimeFormat and Collator can be called without new (ECMA-402), the other constructors cannot.
  const callable = Class => {
    const constructor = { [Class.name]: function (locales, options) {
      return Reflect.construct(Class, [locales, options], new.target ?? constructor);
    } }[Class.name];
    constructor.prototype = Class.prototype;
    Object.defineProperty(Class.prototype, 'constructor', { value: constructor, writable: true, configurable: true });
    define(constructor, { supportedLocalesOf });
    return constructor;
  };

  const supportedValues = { calendar: ['gregory'], collation: ['default'], currency: Object.keys(currencySymbols).sort(),
    numberingSystem: ['latn'], timeZone: ['UTC'], unit: Object.keys(unitNames).sort() };
  const Intl = {};
  define(Intl, { Collator: callable(Collator), DateTimeFormat: callable(DateTimeFormat), ListFormat, Locale,
    NumberFormat: callable(NumberFormat), PluralRules, RelativeTimeFormat,
    getCanonicalLocales: locales => canonicalLocales(locales),
    supportedValuesOf: key => {
      const values = supportedValues[String(key)];
      if (values === undefined) throw new RangeError(`Invalid key : ${String(key)}`);
      return values.slice();
    } });
  Object.defineProperty(Intl, Symbol.toStringTag, { value: 'Intl', configurable: true });

  // The locale methods of the built-ins (natives that call these ones): QuickJS-ng ignores their locales and options.
  const defaultFormats = {};
  // A format of 'Kind' for 'locales' and 'options', with 'defaults' (can be undefined) when the options have none of the
  // fields of 'fields': the defaults of the locale methods of Date.
  const formatter = (name, Kind, locales, options, defaults, fields) => {
    if (locales === undefined && options === undefined) return defaultFormats[name] ??= new Kind(undefined, defaults);
    const defaulted = defaults !== undefined && !Object.keys(Object(options)).some(key => fields.has(key));
    return new Kind(locales, defaulted ? { ...options, ...defaults } : options);
  };
  const dateFields = new Set(['weekday', 'era', 'year', 'month', 'day', 'hour', 'minute', 'second', 'dateStyle',
    'timeStyle', 'dayPeriod', 'fractionalSecondDigits']);
  const kDate = { year: 'numeric', month: 'numeric', day: 'numeric' };
  const kTime = { hour: 'numeric', minute: '2-digit', second: '2-digit' };
  const dateMethod = (name, defaults) => function (locales, options) {
    if (!(this instanceof Date)) throw new TypeError('not a Date');
    return formatter(name, DateTimeFormat, locales, options, defaults, dateFields).format(this);
  };
  return { Intl,
    numberToLocaleString(locales, options) {
      return formatter('number', NumberFormat, locales, options).format(Number.prototype.valueOf.call(this));
    },
    localeCompare(that, locales, options) {
      if (this == null) throw new TypeError('String.prototype.localeCompare called on null or undefined');
      return formatter('collator', Collator, locales, options).compare(String(this), String(that));
    },
    dateToLocaleString: dateMethod('dateTime', { ...kDate, ...kTime }),
    dateToLocaleDateString: dateMethod('date', kDate),
    dateToLocaleTimeString: dateMethod('time', kTime),
  };
})
)js";

}  // namespace

std::string_view PreludeSource() noexcept { return kPrelude; }

std::string_view LazyPreludeSource(LazyPrelude part) noexcept {
  switch (part) {
    case LazyPrelude::Network:
      return kNetworkPrelude;
    case LazyPrelude::View:
      return kViewPrelude;
    default:
      assert(part == LazyPrelude::Intl);
      return kIntlPrelude;
  }
}

const char* LazyPreludeName(LazyPrelude part) noexcept {
  switch (part) {
    case LazyPrelude::Network:
      return "aeronet:network";
    case LazyPrelude::View:
      return "aeronet:view";
    default:
      assert(part == LazyPrelude::Intl);
      return "aeronet:intl";
  }
}

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
