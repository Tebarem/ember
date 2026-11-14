// XMLHttpRequest and ProgressEvent (N1b): the JS-written part, evaluated once per realm by impl_xhr.c the
// first time one of the names is read. The transport is fetch (src/bindings/impl_loader_fetch.c), so an app:// handler and
// a network request take the same path, and a streamed body delivers its progress as its chunks arrive. There is no CORS:
// withCredentials picks the credentials mode. The source is a function of one host object (the names it imports) and
// returns its exports.
(function (host) {
  'use strict';

  var EventTargetBase = host.EventTarget;
  var EventBase = host.Event;
  var DOMExc = host.DOMException;
  var AbortCtl = host.AbortController;
  var fetchFn = host.fetch;
  var TextDec = host.TextDecoder;
  var TextEnc = host.TextEncoder;
  var BlobCls = host.Blob;
  var FormDataCls = host.FormData;
  var ParamsCls = host.URLSearchParams;
  var setTimer = host.setTimeout;
  var clearTimer = host.clearTimeout;
  var warn = host.console && host.console.warn;

  function domError(name, message) {
    return new DOMExc(message, name);
  }

  // ------------------------------------------------------------------ ProgressEvent

  class ProgressEvent extends EventBase {
    constructor(type, init) {
      super(type, init);
      var o = init || {};
      this._lengthComputable = !!o.lengthComputable;
      this._loaded = o.loaded === undefined ? 0 : Number(o.loaded) >>> 0;
      this._total = o.total === undefined ? 0 : Number(o.total) >>> 0;
    }
    get lengthComputable() { return this._lengthComputable; }
    get loaded() { return this._loaded; }
    get total() { return this._total; }
  }

  // ------------------------------------------------------------------ state

  var STATES = new WeakMap();    // XMLHttpRequest -> state record
  var UPLOADS = new WeakSet();   // XMLHttpRequestUpload objects that have a listener (the upload listener flag)
  var HANDLERS = new WeakMap();  // target -> { name: { fn, wrap } }

  function stateOf(x) {
    var s = STATES.get(x);
    if (s === undefined) {
      throw new TypeError('Illegal invocation');
    }
    return s;
  }

  function newState() {
    return {
      readyState: 0,
      method: 'GET',
      url: '',
      reqHeaders: [],         // [lowercase name, value] pairs, combined per name
      sendFlag: false,
      token: 0,               // bumped to forget the request in flight (abort, open, a new send, completion)
      ac: null,
      timer: null,
      timeout: 0,
      withCredentials: false,
      responseType: '',
      overrideMime: '',
      status: 0,
      statusText: '',
      responseURL: '',
      headers: null,          // the response Headers (fetch's, set-cookie hidden)
      mime: '',
      decoder: null,
      text: '',
      chunks: [],             // the received bytes (Uint8Array), for arraybuffer and blob
      loaded: 0,
      total: 0,
      lengthComputable: false,
      error: false,
      cacheDone: null,        // the response of arraybuffer, blob and json, computed once at DONE
      uploadComplete: true
    };
  }

  // ------------------------------------------------------------------ events

  function fire(target, type, loaded, total, lengthComputable) {
    target.dispatchEvent(new ProgressEvent(type, {
      lengthComputable: !!lengthComputable,
      loaded: loaded || 0,
      total: total || 0
    }));
  }

  function fireUpload(s, upload, type, loaded, total) {
    if (UPLOADS.has(upload)) {
      fire(upload, type, loaded, total, total > 0);
    }
  }

  function defineHandler(proto, name) {
    Object.defineProperty(proto, 'on' + name, {
      configurable: true,
      enumerable: true,
      get: function () {
        var table = HANDLERS.get(this);
        var h = table && table[name];
        return h ? h.fn : null;
      },
      set: function (fn) {
        var table = HANDLERS.get(this);
        if (table === undefined) {
          table = {};
          HANDLERS.set(this, table);
        }
        var old = table[name];
        if (old) {
          this.removeEventListener(name, old.wrap);
          table[name] = null;
        }
        if (typeof fn === 'function') {
          var wrap = function (e) { return fn.call(this, e); };
          table[name] = { fn: fn, wrap: wrap };
          this.addEventListener(name, wrap);
        }
      }
    });
  }

  // ------------------------------------------------------------------ event targets

  class XMLHttpRequestEventTarget extends EventTargetBase {}
  ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend'].forEach(function (n) {
    defineHandler(XMLHttpRequestEventTarget.prototype, n);
  });

  class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {
    addEventListener(type, callback, options) {
      UPLOADS.add(this);
      return super.addEventListener(type, callback, options);
    }
  }

  // ------------------------------------------------------------------ request helpers

  var FORBIDDEN = {
    'accept-charset': 1, 'accept-encoding': 1, 'access-control-request-headers': 1, 'access-control-request-method': 1,
    'connection': 1, 'content-length': 1, 'cookie': 1, 'cookie2': 1, 'date': 1, 'dnt': 1, 'expect': 1, 'host': 1,
    'keep-alive': 1, 'origin': 1, 'referer': 1, 'set-cookie': 1, 'te': 1, 'trailer': 1, 'transfer-encoding': 1,
    'upgrade': 1, 'via': 1
  };

  function forbiddenHeader(lower) {
    return FORBIDDEN[lower] === 1 || lower.indexOf('proxy-') === 0 || lower.indexOf('sec-') === 0;
  }

  var TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
  var STANDARD = { DELETE: 1, GET: 1, HEAD: 1, OPTIONS: 1, POST: 1, PUT: 1 };

  function isBodyInit(body) {
    return typeof body === 'string' || body instanceof BlobCls || body instanceof ArrayBuffer ||
      ArrayBuffer.isView(body) || (ParamsCls && body instanceof ParamsCls) || (FormDataCls && body instanceof FormDataCls);
  }

  function bodyLength(body) {
    if (typeof body === 'string') {
      return new TextEnc().encode(body).length;
    }
    if (body instanceof BlobCls) {
      return body.size;
    }
    if (body instanceof ArrayBuffer) {
      return body.byteLength;
    }
    if (ArrayBuffer.isView(body)) {
      return body.byteLength;
    }
    return 0;
  }

  // The charset of a Content-Type (or of overrideMimeType) for decoding the text.
  function charsetOf(mime) {
    var m = /;\s*charset\s*=\s*"?([^";]+)"?/i.exec(mime || '');
    return m ? m[1].trim() : '';
  }

  // The response types that keep the received bytes (the others read the text only).
  function keepsBytes(s) {
    return s.responseType === 'arraybuffer' || s.responseType === 'blob' || s.responseType === 'json';
  }

  // A MIME type for overrideMimeType: a type/subtype (token characters) with optional parameters, else SyntaxError.
  function validMime(mime) {
    return /^[!#$%&'*+.^_`|~0-9A-Za-z-]+\/[!#$%&'*+.^_`|~0-9A-Za-z-]+\s*(;.*)?$/.test(String(mime).trim());
  }

  function makeDecoder(label) {
    try {
      return new TextDec(label || 'utf-8');
    } catch (e) {
      return new TextDec('utf-8');
    }
  }

  function clearRequestTimer(s) {
    if (s.timer !== null) {
      clearTimer(s.timer);
      s.timer = null;
    }
  }

  // Forgets the response: what a failed or aborted request leaves behind.
  function clearResponse(s) {
    s.status = 0;
    s.statusText = '';
    s.responseURL = '';
    s.headers = null;
    s.mime = '';
    s.decoder = null;
    s.text = '';
    s.chunks = [];
    s.loaded = 0;
    s.total = 0;
    s.lengthComputable = false;
    s.cacheDone = null;
  }

  function setReadyState(x, s, value) {
    s.readyState = value;
    x.dispatchEvent(new EventBase('readystatechange'));
  }

  // The request error steps (XHR 4.6.4 "request error steps"): done, the response is a network error, then the events.
  function requestError(x, s, kind) {
    s.token++;
    clearRequestTimer(s);
    if (s.ac !== null) {
      var ac = s.ac;
      s.ac = null;
      ac.abort();
    }
    s.readyState = 4;
    s.sendFlag = false;
    s.error = true;
    clearResponse(s);
    x.dispatchEvent(new EventBase('readystatechange'));
    var up = x.upload;
    if (!s.uploadComplete) {
      s.uploadComplete = true;
      fireUpload(s, up, kind, 0, 0);
      fireUpload(s, up, 'loadend', 0, 0);
    }
    fire(x, kind, 0, 0, false);
    fire(x, 'loadend', 0, 0, false);
  }

  // The end of the body (XHR "handle response end-of-body"). A listener that aborts or sends again stops the events here.
  function finish(x, s, token) {
    if (s.token !== token) {
      return;
    }
    clearRequestTimer(s);
    s.ac = null;
    s.readyState = 4;
    s.sendFlag = false;
    if (s.decoder !== null) {
      s.text += s.decoder.decode();
      s.decoder = null;
    }
    x.dispatchEvent(new EventBase('readystatechange'));
    if (s.token !== token) {
      return;
    }
    fire(x, 'progress', s.loaded, s.total, s.lengthComputable);
    fire(x, 'load', s.loaded, s.total, s.lengthComputable);
    fire(x, 'loadend', s.loaded, s.total, s.lengthComputable);
  }

  function readBody(x, s, token, body) {
    var reader = body.getReader();
    function pump() {
      reader.read().then(function (res) {
        if (s.token !== token) {
          return;
        }
        if (res.done) {
          finish(x, s, token);
          return;
        }
        var chunk = res.value;
        if (keepsBytes(s)) {
          s.chunks.push(chunk);
        }
        s.loaded += chunk.byteLength;
        s.text += s.decoder.decode(chunk, { stream: true });
        if (s.readyState === 2) {
          setReadyState(x, s, 3);
          if (s.token !== token) {
            return;
          }
        }
        fire(x, 'progress', s.loaded, s.total, s.lengthComputable);
        if (s.token === token) {
          pump();
        }
      }, function () {
        if (s.token === token) {
          requestError(x, s, 'error');
        }
      });
    }
    pump();
  }

  function onHead(x, s, token, r) {
    if (s.token !== token) {
      return;
    }
    s.status = r.status;
    s.statusText = r.statusText;
    s.responseURL = r.url.split('#')[0];
    s.headers = r.headers;
    s.mime = r.headers.get('content-type') || '';
    var length = r.headers.get('content-length');
    s.lengthComputable = false;
    s.total = 0;
    if (length !== null && r.headers.get('content-encoding') === null && /^\d+$/.test(length.trim())) {
      s.total = Number(length.trim());
      s.lengthComputable = true;
    }
    var label = charsetOf(s.overrideMime) || charsetOf(s.mime);
    s.decoder = makeDecoder(label);
    setReadyState(x, s, 2);
    if (s.token !== token) {
      return;
    }
    if (!r.body) {
      finish(x, s, token);
      return;
    }
    readBody(x, s, token, r.body);
  }

  // ------------------------------------------------------------------ XMLHttpRequest

  class XMLHttpRequest extends XMLHttpRequestEventTarget {
    constructor() {
      super();
      STATES.set(this, newState());
      this.upload = new XMLHttpRequestUpload();
    }

    open(method, url, async, user, password) {
      var s = stateOf(this);
      if (!TOKEN.test(String(method))) {
        throw domError('SyntaxError', 'Failed to execute \'open\' on \'XMLHttpRequest\': \'' + method + '\' is not a valid HTTP method.');
      }
      var upper = String(method).toUpperCase();
      if (upper === 'CONNECT' || upper === 'TRACE' || upper === 'TRACK') {
        throw domError('SecurityError', 'Failed to execute \'open\' on \'XMLHttpRequest\': \'' + method + '\' HTTP method is unsupported.');
      }
      if (STANDARD[upper] === 1) {
        method = upper;
      }
      var base = typeof location === 'object' && location !== null ? location.href : undefined;
      var parsed;
      try {
        parsed = new URL(String(url), base);
      } catch (e) {
        throw domError('SyntaxError', 'Failed to execute \'open\' on \'XMLHttpRequest\': Invalid URL');
      }
      if (async === false) {
        if (warn) {
          warn('XMLHttpRequest: synchronous requests are not supported (open with async false); use an asynchronous request');
        }
        throw domError('InvalidAccessError', 'Failed to execute \'open\' on \'XMLHttpRequest\': Synchronous requests are not supported by this engine.');
      }
      // A request in flight is terminated silently.
      s.token++;
      clearRequestTimer(s);
      if (s.ac !== null) {
        var old = s.ac;
        s.ac = null;
        old.abort();
      }
      s.method = method;
      s.url = parsed.href;
      s.reqHeaders = [];
      s.sendFlag = false;
      s.error = false;
      s.uploadComplete = true;
      clearResponse(s);
      if (s.readyState !== 1) {
        setReadyState(this, s, 1);
      }
    }

    setRequestHeader(name, value) {
      var s = stateOf(this);
      if (s.readyState !== 1 || s.sendFlag) {
        throw domError('InvalidStateError', 'Failed to execute \'setRequestHeader\' on \'XMLHttpRequest\': The object is in an invalid state.');
      }
      name = String(name);
      if (!TOKEN.test(name)) {
        throw domError('SyntaxError', 'Failed to execute \'setRequestHeader\' on \'XMLHttpRequest\': \'' + name + '\' is not a valid HTTP header field name.');
      }
      value = String(value).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, '');
      if (/[\0\r\n]/.test(value)) {
        throw domError('SyntaxError', 'Failed to execute \'setRequestHeader\' on \'XMLHttpRequest\': Invalid header value.');
      }
      var lower = name.toLowerCase();
      if (forbiddenHeader(lower)) {
        return;
      }
      for (var i = 0; i < s.reqHeaders.length; i++) {
        if (s.reqHeaders[i][0] === lower) {
          s.reqHeaders[i][1] = s.reqHeaders[i][1] + ', ' + value;
          return;
        }
      }
      s.reqHeaders.push([lower, value]);
    }

    send(body) {
      var x = this;
      var s = stateOf(x);
      if (s.readyState !== 1 || s.sendFlag) {
        throw domError('InvalidStateError', 'Failed to execute \'send\' on \'XMLHttpRequest\': The object is in an invalid state.');
      }
      var hasBody = body !== undefined && body !== null && s.method !== 'GET' && s.method !== 'HEAD';
      var init = { method: s.method, headers: s.reqHeaders.map(function (p) { return [p[0], p[1]]; }),
                   credentials: s.withCredentials ? 'include' : 'same-origin', redirect: 'follow' };
      var total = 0;
      if (hasBody) {
        init.body = isBodyInit(body) ? body : String(body);
        total = bodyLength(init.body);
      }
      s.sendFlag = true;
      s.error = false;
      s.uploadComplete = !hasBody;
      clearResponse(s);
      var token = ++s.token;
      var ac = new AbortCtl();
      s.ac = ac;
      init.signal = ac.signal;
      if (s.timeout > 0) {
        s.timer = setTimer(function () {
          s.timer = null;
          if (s.token === token) {
            requestError(x, s, 'timeout');
          }
        }, s.timeout);
      }
      fire(x, 'loadstart', 0, 0, false);
      if (s.token !== token) {
        return;
      }
      if (hasBody) {
        s.uploadComplete = true;
        fireUpload(s, x.upload, 'loadstart', 0, total);
        fireUpload(s, x.upload, 'progress', total, total);
        fireUpload(s, x.upload, 'load', total, total);
        fireUpload(s, x.upload, 'loadend', total, total);
        if (s.token !== token) {
          return;
        }
      }
      fetchFn(s.url, init).then(function (r) {
        onHead(x, s, token, r);
      }, function () {
        if (s.token === token) {
          requestError(x, s, 'error');
        }
      });
    }

    abort() {
      var s = stateOf(this);
      if ((s.readyState === 1 && s.sendFlag) || s.readyState === 2 || s.readyState === 3) {
        requestError(this, s, 'abort');
      }
      if (s.readyState === 4) {
        s.readyState = 0;
        clearResponse(s);
      }
    }

    getResponseHeader(name) {
      var s = stateOf(this);
      if (s.headers === null) {
        return null;
      }
      return s.headers.get(String(name));
    }

    getAllResponseHeaders() {
      var s = stateOf(this);
      if (s.headers === null) {
        return '';
      }
      var out = '';
      s.headers.forEach(function (value, key) {
        out += key + ': ' + value + '\r\n';
      });
      return out;
    }

    overrideMimeType(mime) {
      var s = stateOf(this);
      if (s.readyState === 3 || s.readyState === 4) {
        throw domError('InvalidStateError', 'Failed to execute \'overrideMimeType\' on \'XMLHttpRequest\': MIME type cannot be overridden while loading.');
      }
      var value = String(mime);
      if (!validMime(value)) {
        throw new SyntaxError("Failed to execute 'overrideMimeType' on 'XMLHttpRequest': Invalid MIME type '" + value + "'.");
      }
      s.overrideMime = value;
    }

    get readyState() { return stateOf(this).readyState; }

    get status() {
      var s = stateOf(this);
      return s.readyState === 0 || s.readyState === 1 || s.error ? 0 : s.status;
    }

    get statusText() {
      var s = stateOf(this);
      return s.readyState === 0 || s.readyState === 1 || s.error ? '' : s.statusText;
    }

    get responseURL() { return stateOf(this).responseURL; }

    get responseText() {
      var s = stateOf(this);
      if (s.responseType !== '' && s.responseType !== 'text') {
        throw domError('InvalidStateError', 'Failed to read the \'responseText\' property from \'XMLHttpRequest\': The value is only accessible if the object\'s \'responseType\' is \'\' or \'text\'.');
      }
      return s.text;
    }

    get response() {
      var s = stateOf(this);
      switch (s.responseType) {
        case '':
        case 'text':
          return s.text;
        case 'arraybuffer':
          // The bytes, once DONE (null before, and after an error).
          if (s.readyState !== 4 || s.error) {
            return null;
          }
          if (s.cacheDone === null) {
            s.cacheDone = concatBytes(s.chunks).buffer;
          }
          return s.cacheDone;
        case 'blob':
          if (s.readyState !== 4 || s.error) {
            return null;
          }
          if (s.cacheDone === null) {
            s.cacheDone = new BlobCls(s.chunks, { type: s.overrideMime || s.mime });
          }
          return s.cacheDone;
        case 'json':
          // The UTF-8 decoding of the bytes (a BOM is dropped), parsed once DONE; null when it does not parse.
          if (s.readyState !== 4 || s.error) {
            return null;
          }
          if (s.cacheDone === null) {
            try {
              s.cacheDone = JSON.parse(new TextDec('utf-8').decode(concatBytes(s.chunks)));
            } catch (e) {
              s.cacheDone = null;
            }
          }
          return s.cacheDone;
        default:
          return null;
      }
    }

    get responseType() { return stateOf(this).responseType; }

    set responseType(value) {
      var s = stateOf(this);
      if (s.readyState === 3 || s.readyState === 4) {
        throw domError('InvalidStateError', 'Failed to set the \'responseType\' property on \'XMLHttpRequest\': The response type cannot be changed while loading.');
      }
      value = String(value);
      if (value === 'document') {
        throw domError('NotSupportedError', 'Failed to set the \'responseType\' property on \'XMLHttpRequest\': The \'document\' response type is not supported.');
      }
      if (value !== '' && value !== 'text' && value !== 'json' && value !== 'arraybuffer' && value !== 'blob') {
        throw new TypeError('Failed to set the \'responseType\' property on \'XMLHttpRequest\': The provided value \'' + value + '\' is not a valid enum value.');
      }
      s.responseType = value;
    }

    get timeout() { return stateOf(this).timeout; }

    set timeout(value) {
      var s = stateOf(this);
      var n = Number(value);
      s.timeout = n !== n || n < 0 ? 0 : Math.min(Math.floor(n), 4294967295);
    }

    get withCredentials() { return stateOf(this).withCredentials; }

    set withCredentials(value) {
      var s = stateOf(this);
      if ((s.readyState !== 0 && s.readyState !== 1) || s.sendFlag) {
        throw domError('InvalidStateError', 'Failed to set the \'withCredentials\' property on \'XMLHttpRequest\': The object is in an invalid state.');
      }
      s.withCredentials = !!value;
    }
  }

  function concatBytes(list) {
    var n = 0;
    for (var i = 0; i < list.length; i++) {
      n += list[i].byteLength;
    }
    var out = new Uint8Array(n);
    var at = 0;
    for (var j = 0; j < list.length; j++) {
      out.set(list[j], at);
      at += list[j].byteLength;
    }
    return out;
  }

  defineHandler(XMLHttpRequest.prototype, 'readystatechange');

  var CONSTANTS = { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 };
  Object.keys(CONSTANTS).forEach(function (k) {
    Object.defineProperty(XMLHttpRequest, k, { value: CONSTANTS[k], enumerable: true });
    Object.defineProperty(XMLHttpRequest.prototype, k, { value: CONSTANTS[k], enumerable: true });
  });

  return {
    XMLHttpRequest: XMLHttpRequest,
    XMLHttpRequestUpload: XMLHttpRequestUpload,
    XMLHttpRequestEventTarget: XMLHttpRequestEventTarget,
    ProgressEvent: ProgressEvent
  };
})
