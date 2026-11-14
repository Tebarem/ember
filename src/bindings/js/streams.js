// The Streams Standard pieces that sit on top of the native ReadableStream (src/bindings/impl_loader_stream.c): WritableStream
// and its writer and controller, TransformStream and its controller, pipeTo and pipeThrough, TextEncoderStream and
// TextDecoderStream (Encoding Standard), and the helpers that make a ReadableStream transferable through a MessagePort.
// W7f. Embedded with #embed by impl_loader_prelude.c and evaluated once per realm, the first time any of it is used; the
// completion value is a function that takes the host object (a few native helpers) and returns the exports.
//
// Every abstract operation works on a plain "internal" object; the public classes hold theirs in a private field, so a
// page cannot reach or corrupt the state. The names follow the Streams Standard.
(function (host) {
'use strict';
const RS = host.ReadableStream;
const TE = host.TextEncoder;
const TD = host.TextDecoder;
const AbortSignalC = host.AbortSignal;
const AbortControllerC = host.AbortController;
const DOMExceptionC = host.DOMException;
const MessageChannelC = host.MessageChannel;
const Promise_ = Promise;
const NumberIsNaN = Number.isNaN;
const ObjectCreate = Object.create;
const Reflect_apply = Reflect.apply;
const ArrayBuffer_ = ArrayBuffer;
const isView = ArrayBuffer.isView;
const noop = () => {};

function isObject(v) { return (typeof v === 'object' && v !== null) || typeof v === 'function'; }
function newDeferred() {
  const d = { settled: false };
  d.promise = new Promise_((resolve, reject) => {
    d.resolve = (v) => { d.settled = true; resolve(v); };
    d.reject = (e) => { d.settled = true; reject(e); };
  });
  return d;
}
function resolved(v) { return Promise_.resolve(v); }
function rejected(e) { return Promise_.reject(e); }
function markHandled(p) { p.then(undefined, noop); return p; }
// then() whose derived promise never reports an unhandled rejection (the standard's "upon" operations).
function upon(p, onF, onR) { const d = p.then(onF, onR); d.then(undefined, noop); }
function uponF(p, f) { upon(p, f, undefined); }
function uponR(p, f) { upon(p, undefined, f); }
function transformPromise(p, f, r) { return p.then(f, r); }
// PromiseCall(F, V, args): calls and always returns a promise.
function promiseCall(f, v, args) {
  try { return resolved(Reflect_apply(f, v, args)); } catch (e) { return rejected(e); }
}
function validateFn(fn, name) {
  if (fn !== undefined && typeof fn !== 'function') throw new TypeError(name + ' is not a function');
}
function validateAndNormalizeHighWaterMark(hwm) {
  hwm = Number(hwm);
  if (NumberIsNaN(hwm) || hwm < 0) throw new RangeError('highWaterMark must be a non-negative number');
  return hwm;
}
function extractHighWaterMark(strategy, def) {
  const hwm = strategy.highWaterMark;
  if (hwm === undefined) return def;
  return validateAndNormalizeHighWaterMark(hwm);
}
function extractSizeAlgorithm(strategy) {
  const size = strategy.size;
  if (size === undefined) return () => 1;
  validateFn(size, 'size');
  return (chunk) => size.call(undefined, chunk);
}
function convertStrategy(strategy) {
  if (strategy === undefined || strategy === null) return {};
  if (!isObject(strategy)) throw new TypeError('The strategy is not an object');
  return strategy;
}

// ------------------------------------------------------------------------------------------------ WritableStream

const kStreamBrand = Symbol('ws');

function WS_new() {
  return {
    pub: null, state: 'writable', storedError: undefined, writer: undefined, controller: undefined,
    inFlightWriteRequest: undefined, closeRequest: undefined, inFlightCloseRequest: undefined,
    pendingAbortRequest: undefined, backpressure: false, writeRequests: [],
  };
}
function WS_isLocked(s) { return s.writer !== undefined; }
function WS_hasOperationMarkedInFlight(s) { return s.inFlightWriteRequest !== undefined || s.inFlightCloseRequest !== undefined; }
function WS_closeQueuedOrInFlight(s) { return s.closeRequest !== undefined || s.inFlightCloseRequest !== undefined; }

function WS_abort(s, reason) {
  if (s.state === 'closed' || s.state === 'errored') return resolved(undefined);
  s.controller.abortController.abort(reason);
  if (s.state === 'closed' || s.state === 'errored') return resolved(undefined);
  if (s.pendingAbortRequest !== undefined) return s.pendingAbortRequest.promise;
  let wasAlreadyErroring = false;
  if (s.state === 'erroring') { wasAlreadyErroring = true; reason = undefined; }
  const d = newDeferred();
  s.pendingAbortRequest = { promise: d.promise, resolve: d.resolve, reject: d.reject, reason, wasAlreadyErroring };
  if (!wasAlreadyErroring) WS_startErroring(s, reason);
  return d.promise;
}
function WS_close(s) {
  if (s.state === 'closed' || s.state === 'errored') {
    return rejected(new TypeError('The stream (in ' + s.state + ' state) is not in the writable state and cannot be closed'));
  }
  const d = newDeferred();
  s.closeRequest = d;
  const writer = s.writer;
  if (writer !== undefined && s.backpressure && s.state === 'writable') writer.readyDeferred.resolve(undefined);
  WSC_close(s.controller);
  return d.promise;
}
function WS_addWriteRequest(s) {
  const d = newDeferred();
  s.writeRequests.push(d);
  return d.promise;
}
function WS_dealWithRejection(s, error) {
  if (s.state === 'writable') { WS_startErroring(s, error); return; }
  WS_finishErroring(s);
}
function WS_startErroring(s, reason) {
  const controller = s.controller;
  s.state = 'erroring';
  s.storedError = reason;
  const writer = s.writer;
  if (writer !== undefined) WSW_ensureReadyPromiseRejected(writer, reason);
  if (!WS_hasOperationMarkedInFlight(s) && controller.started) WS_finishErroring(s);
}
function WS_finishErroring(s) {
  s.state = 'errored';
  s.controller.errorSteps();
  const storedError = s.storedError;
  for (const wr of s.writeRequests) wr.reject(storedError);
  s.writeRequests = [];
  if (s.pendingAbortRequest === undefined) { WS_rejectCloseAndClosedPromiseIfNeeded(s); return; }
  const abortRequest = s.pendingAbortRequest;
  s.pendingAbortRequest = undefined;
  if (abortRequest.wasAlreadyErroring) {
    abortRequest.reject(storedError);
    WS_rejectCloseAndClosedPromiseIfNeeded(s);
    return;
  }
  const promise = s.controller.abortSteps(abortRequest.reason);
  upon(promise, () => { abortRequest.resolve(); WS_rejectCloseAndClosedPromiseIfNeeded(s); },
    (reason) => { abortRequest.reject(reason); WS_rejectCloseAndClosedPromiseIfNeeded(s); });
}
function WS_finishInFlightWrite(s) {
  s.inFlightWriteRequest.resolve(undefined);
  s.inFlightWriteRequest = undefined;
}
function WS_finishInFlightWriteWithError(s, error) {
  s.inFlightWriteRequest.reject(error);
  s.inFlightWriteRequest = undefined;
  WS_dealWithRejection(s, error);
}
function WS_finishInFlightClose(s) {
  s.inFlightCloseRequest.resolve(undefined);
  s.inFlightCloseRequest = undefined;
  if (s.state === 'erroring') {
    s.storedError = undefined;
    if (s.pendingAbortRequest !== undefined) {
      s.pendingAbortRequest.resolve();
      s.pendingAbortRequest = undefined;
    }
  }
  s.state = 'closed';
  const writer = s.writer;
  if (writer !== undefined) writer.closedDeferred.resolve(undefined);
}
function WS_finishInFlightCloseWithError(s, error) {
  s.inFlightCloseRequest.reject(error);
  s.inFlightCloseRequest = undefined;
  if (s.pendingAbortRequest !== undefined) {
    s.pendingAbortRequest.reject(error);
    s.pendingAbortRequest = undefined;
  }
  WS_dealWithRejection(s, error);
}
function WS_markCloseRequestInFlight(s) {
  s.inFlightCloseRequest = s.closeRequest;
  s.closeRequest = undefined;
}
function WS_markFirstWriteRequestInFlight(s) {
  s.inFlightWriteRequest = s.writeRequests.shift();
}
function WS_rejectCloseAndClosedPromiseIfNeeded(s) {
  if (s.closeRequest !== undefined) {
    s.closeRequest.reject(s.storedError);
    s.closeRequest = undefined;
  }
  const writer = s.writer;
  if (writer !== undefined) {
    writer.closedDeferred.reject(s.storedError);
    markHandled(writer.closedDeferred.promise);
  }
}
function WS_updateBackpressure(s, backpressure) {
  const writer = s.writer;
  if (writer !== undefined && backpressure !== s.backpressure) {
    if (backpressure) writer.readyDeferred = newDeferred();
    else writer.readyDeferred.resolve(undefined);
  }
  s.backpressure = backpressure;
}

// --- writer
function WSW_new(s) {
  if (WS_isLocked(s)) throw new TypeError('This stream has already been locked for exclusive writing by another writer');
  const w = { pub: null, stream: s, closedDeferred: undefined, readyDeferred: undefined };
  s.writer = w;
  const state = s.state;
  if (state === 'writable') {
    if (!WS_closeQueuedOrInFlight(s) && s.backpressure) w.readyDeferred = newDeferred();
    else { w.readyDeferred = newDeferred(); w.readyDeferred.resolve(undefined); }
    w.closedDeferred = newDeferred();
  } else if (state === 'erroring') {
    w.readyDeferred = newDeferred(); w.readyDeferred.reject(s.storedError); markHandled(w.readyDeferred.promise);
    w.closedDeferred = newDeferred();
  } else if (state === 'closed') {
    w.readyDeferred = newDeferred(); w.readyDeferred.resolve(undefined);
    w.closedDeferred = newDeferred(); w.closedDeferred.resolve(undefined);
  } else {
    const stored = s.storedError;
    w.readyDeferred = newDeferred(); w.readyDeferred.reject(stored); markHandled(w.readyDeferred.promise);
    w.closedDeferred = newDeferred(); w.closedDeferred.reject(stored); markHandled(w.closedDeferred.promise);
  }
  return w;
}
function WSW_abort(w, reason) { return WS_abort(w.stream, reason); }
function WSW_close(w) { return WS_close(w.stream); }
function WSW_closeWithErrorPropagation(w) {
  const s = w.stream;
  const state = s.state;
  if (WS_closeQueuedOrInFlight(s) || state === 'closed') return resolved(undefined);
  if (state === 'errored') return rejected(s.storedError);
  return WSW_close(w);
}
function WSW_ensureClosedPromiseRejected(w, error) {
  if (w.closedDeferred.settled) w.closedDeferred = newDeferred();
  w.closedDeferred.reject(error);
  markHandled(w.closedDeferred.promise);
}
function WSW_ensureReadyPromiseRejected(w, error) {
  if (w.readyDeferred.settled) w.readyDeferred = newDeferred();
  w.readyDeferred.reject(error);
  markHandled(w.readyDeferred.promise);
}
function WSW_getDesiredSize(w) {
  const s = w.stream;
  const state = s.state;
  if (state === 'errored' || state === 'erroring') return null;
  if (state === 'closed') return 0;
  return WSC_getDesiredSize(s.controller);
}
function WSW_release(w) {
  const s = w.stream;
  const released = new TypeError('Writer was released and can no longer be used to monitor the stream\'s closedness');
  WSW_ensureReadyPromiseRejected(w, released);
  WSW_ensureClosedPromiseRejected(w, released);
  s.writer = undefined;
  w.stream = undefined;
}
function WSW_write(w, chunk) {
  const s = w.stream;
  const controller = s.controller;
  const chunkSize = WSC_getChunkSize(controller, chunk);
  if (s !== w.stream) return rejected(new TypeError('Cannot write to a stream using a released writer'));
  const state = s.state;
  if (state === 'errored') return rejected(s.storedError);
  if (WS_closeQueuedOrInFlight(s) || state === 'closed') {
    return rejected(new TypeError('The stream is closing or closed and cannot be written to'));
  }
  if (state === 'erroring') return rejected(s.storedError);
  const promise = WS_addWriteRequest(s);
  WSC_write(controller, chunk, chunkSize);
  return promise;
}

// --- controller
function WSC_setUp(s, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, hwm, sizeAlgorithm) {
  controller.stream = s;
  s.controller = controller;
  controller.queue = []; controller.queueTotalSize = 0;
  controller.abortController = new AbortControllerC();
  controller.started = false;
  controller.strategySizeAlgorithm = sizeAlgorithm;
  controller.strategyHWM = hwm;
  controller.writeAlgorithm = writeAlgorithm;
  controller.closeAlgorithm = closeAlgorithm;
  controller.abortAlgorithm = abortAlgorithm;
  WS_updateBackpressure(s, WSC_getBackpressure(controller));
  const startResult = startAlgorithm();
  upon(resolved(startResult), () => {
    controller.started = true;
    WSC_advanceQueueIfNeeded(controller);
  }, (r) => {
    controller.started = true;
    WS_dealWithRejection(s, r);
  });
}
function WSC_makeInternal() {
  const c = {
    pub: null, stream: undefined, queue: undefined, queueTotalSize: 0, abortController: undefined, started: false,
    strategySizeAlgorithm: undefined, strategyHWM: 0, writeAlgorithm: undefined, closeAlgorithm: undefined,
    abortAlgorithm: undefined,
  };
  c.abortSteps = (reason) => {
    const result = c.abortAlgorithm(reason);
    WSC_clearAlgorithms(c);
    return result;
  };
  c.errorSteps = () => { c.queue = []; c.queueTotalSize = 0; };
  return c;
}
function WSC_setUpFromSink(s, sink, sinkDict, hwm, sizeAlgorithm) {
  const controller = WSC_makeInternal();
  controller.pub = ObjectCreate(WritableStreamDefaultController.prototype);
  ctlSlots.set(controller.pub, controller);
  const startAlgorithm = sinkDict.start !== undefined ? () => sinkDict.start.call(sink, controller.pub) : () => undefined;
  const writeAlgorithm = sinkDict.write !== undefined ? (chunk) => promiseCall(sinkDict.write, sink, [chunk, controller.pub])
    : () => resolved(undefined);
  const closeAlgorithm = sinkDict.close !== undefined ? () => promiseCall(sinkDict.close, sink, []) : () => resolved(undefined);
  const abortAlgorithm = sinkDict.abort !== undefined ? (reason) => promiseCall(sinkDict.abort, sink, [reason])
    : () => resolved(undefined);
  WSC_setUp(s, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, hwm, sizeAlgorithm);
}
function WSC_clearAlgorithms(c) {
  c.writeAlgorithm = undefined; c.closeAlgorithm = undefined; c.abortAlgorithm = undefined; c.strategySizeAlgorithm = undefined;
}
function WSC_close(c) {
  c.queue.push({ close: true, size: 0 });
  WSC_advanceQueueIfNeeded(c);
}
function WSC_getChunkSize(c, chunk) {
  if (c.strategySizeAlgorithm === undefined) return 1;
  try { return c.strategySizeAlgorithm(chunk); } catch (e) { WSC_errorIfNeeded(c, e); return 1; }
}
function WSC_getDesiredSize(c) { return c.strategyHWM - c.queueTotalSize; }
function WSC_write(c, chunk, chunkSize) {
  try {
    chunkSize = Number(chunkSize);
    if (NumberIsNaN(chunkSize) || chunkSize < 0 || chunkSize === Infinity) throw new RangeError('The return value of a queuing strategy\'s size function must be a finite, non-NaN, non-negative number');
    c.queue.push({ value: chunk, size: chunkSize });
    c.queueTotalSize += chunkSize;
  } catch (e) {
    WSC_errorIfNeeded(c, e);
    return;
  }
  const s = c.stream;
  if (!WS_closeQueuedOrInFlight(s) && s.state === 'writable') WS_updateBackpressure(s, WSC_getBackpressure(c));
  WSC_advanceQueueIfNeeded(c);
}
function WSC_getBackpressure(c) { return WSC_getDesiredSize(c) <= 0; }
function WSC_advanceQueueIfNeeded(c) {
  const s = c.stream;
  if (!c.started) return;
  if (s.inFlightWriteRequest !== undefined) return;
  const state = s.state;
  if (state === 'closed' || state === 'errored') return;
  if (state === 'erroring') { WS_finishErroring(s); return; }
  if (c.queue.length === 0) return;
  const value = c.queue[0];
  if (value.close) WSC_processClose(c); else WSC_processWrite(c, value.value);
}
function WSC_errorIfNeeded(c, error) {
  if (c.stream.state === 'writable') WSC_error(c, error);
}
function WSC_processClose(c) {
  const s = c.stream;
  WS_markCloseRequestInFlight(s);
  c.queue.shift();
  const sinkClosePromise = c.closeAlgorithm();
  WSC_clearAlgorithms(c);
  upon(sinkClosePromise, () => WS_finishInFlightClose(s), (r) => WS_finishInFlightCloseWithError(s, r));
}
function WSC_processWrite(c, chunk) {
  const s = c.stream;
  WS_markFirstWriteRequestInFlight(s);
  const sinkWritePromise = c.writeAlgorithm(chunk);
  upon(sinkWritePromise, () => {
    WS_finishInFlightWrite(s);
    const state = s.state;
    const dq = c.queue.shift();
    c.queueTotalSize -= dq.size;
    if (c.queueTotalSize < 0) c.queueTotalSize = 0;
    if (!WS_closeQueuedOrInFlight(s) && state === 'writable') WS_updateBackpressure(s, WSC_getBackpressure(c));
    WSC_advanceQueueIfNeeded(c);
  }, (r) => {
    if (s.state === 'writable') WSC_clearAlgorithms(c);
    WS_finishInFlightWriteWithError(s, r);
  });
}
function WSC_error(c, error) {
  const s = c.stream;
  WSC_clearAlgorithms(c);
  WS_startErroring(s, error);
}


// --- public classes
const wsSlots = new WeakMap();
const writerSlots = new WeakMap();
const ctlSlots = new WeakMap();

function wsOf(o, name) {
  const s = isObject(o) ? wsSlots.get(o) : undefined;
  if (s === undefined) throw new TypeError('Illegal invocation: ' + name + ' is not a WritableStream');
  return s;
}
function writerOf(o) {
  const s = isObject(o) ? writerSlots.get(o) : undefined;
  if (s === undefined) throw new TypeError('Illegal invocation: not a WritableStreamDefaultWriter');
  return s;
}
function wctlOf(o) {
  const s = isObject(o) ? ctlSlots.get(o) : undefined;
  if (s === undefined) throw new TypeError('Illegal invocation: not a WritableStreamDefaultController');
  return s;
}

function convertSink(sink) {
  if (sink === undefined) return {};
  if (!isObject(sink)) throw new TypeError('The underlying sink is not an object');
  const dict = {};
  dict.abort = sink.abort; validateFn(dict.abort, 'abort');
  dict.close = sink.close; validateFn(dict.close, 'close');
  dict.start = sink.start; validateFn(dict.start, 'start');
  dict.type = sink.type;
  dict.write = sink.write; validateFn(dict.write, 'write');
  return dict;
}

class WritableStream {
  constructor(underlyingSink = undefined, strategy = undefined) {
    const sinkOrNull = underlyingSink === undefined ? null : underlyingSink;
    const dict = convertSink(sinkOrNull === null ? undefined : sinkOrNull);
    strategy = convertStrategy(strategy);
    const s = WS_new();
    s.pub = this;
    wsSlots.set(this, s);
    if (dict.type !== undefined) throw new RangeError('Invalid type is specified');
    const sizeAlgorithm = extractSizeAlgorithm(strategy);
    const hwm = extractHighWaterMark(strategy, 1);
    WSC_setUpFromSink(s, sinkOrNull === null ? {} : sinkOrNull, dict, hwm, sizeAlgorithm);
  }
  get locked() { return WS_isLocked(wsOf(this, 'locked')); }
  abort(reason = undefined) {
    let s;
    try { s = wsOf(this, 'abort'); } catch (e) { return rejected(e); }
    if (WS_isLocked(s)) return rejected(new TypeError('Cannot abort a stream that already has a writer'));
    return WS_abort(s, reason);
  }
  close() {
    let s;
    try { s = wsOf(this, 'close'); } catch (e) { return rejected(e); }
    if (WS_isLocked(s)) return rejected(new TypeError('Cannot close a stream that already has a writer'));
    if (WS_closeQueuedOrInFlight(s)) return rejected(new TypeError('Cannot close an already-closing stream'));
    return WS_close(s);
  }
  getWriter() {
    const s = wsOf(this, 'getWriter');
    return WS_acquireWriter(s);
  }
}

function WS_acquireWriter(s) {
  const pub = ObjectCreate(WritableStreamDefaultWriter.prototype);
  const w = WSW_new(s);
  w.pub = pub;
  writerSlots.set(pub, w);
  return w.pub;
}
function WS_acquireInternalWriter(s) {
  return writerSlots.get(WS_acquireWriter(s));
}

class WritableStreamDefaultWriter {
  constructor(stream) {
    const s = wsOf(stream, 'WritableStreamDefaultWriter');
    const w = WSW_new(s);
    w.pub = this;
    writerSlots.set(this, w);
  }
  get closed() {
    let w;
    try { w = writerOf(this); } catch (e) { return rejected(e); }
    return w.closedDeferred.promise;
  }
  get desiredSize() {
    const w = writerOf(this);
    if (w.stream === undefined) throw new TypeError('The writer has been released and can no longer be used');
    return WSW_getDesiredSize(w);
  }
  get ready() {
    let w;
    try { w = writerOf(this); } catch (e) { return rejected(e); }
    return w.readyDeferred.promise;
  }
  abort(reason = undefined) {
    let w;
    try { w = writerOf(this); } catch (e) { return rejected(e); }
    if (w.stream === undefined) return rejected(new TypeError('Cannot abort a stream using a released writer'));
    return WSW_abort(w, reason);
  }
  close() {
    let w;
    try { w = writerOf(this); } catch (e) { return rejected(e); }
    const s = w.stream;
    if (s === undefined) return rejected(new TypeError('Cannot close a stream using a released writer'));
    if (WS_closeQueuedOrInFlight(s)) return rejected(new TypeError('Cannot close an already-closing stream'));
    return WSW_close(w);
  }
  releaseLock() {
    const w = writerOf(this);
    if (w.stream === undefined) return;
    WSW_release(w);
  }
  write(chunk = undefined) {
    let w;
    try { w = writerOf(this); } catch (e) { return rejected(e); }
    if (w.stream === undefined) return rejected(new TypeError('Cannot write to a stream using a released writer'));
    return WSW_write(w, chunk);
  }
}

class WritableStreamDefaultController {
  constructor() { throw new TypeError('Illegal constructor'); }
  get signal() { return wctlOf(this).abortController.signal; }
  error(e = undefined) {
    const c = wctlOf(this);
    if (c.stream.state !== 'writable') return;
    WSC_error(c, e);
  }
}

for (const [C, names] of [
  [WritableStream, ['locked', 'abort', 'close', 'getWriter']],
  [WritableStreamDefaultWriter, ['closed', 'desiredSize', 'ready', 'abort', 'close', 'releaseLock', 'write']],
  [WritableStreamDefaultController, ['signal', 'error']],
]) {
  for (const n of names) {
    const d = Object.getOwnPropertyDescriptor(C.prototype, n);
    d.enumerable = true;
    Object.defineProperty(C.prototype, n, d);
  }
}
Object.defineProperty(WritableStream.prototype, Symbol.toStringTag, { value: 'WritableStream', configurable: true });
Object.defineProperty(WritableStreamDefaultWriter.prototype, Symbol.toStringTag, { value: 'WritableStreamDefaultWriter', configurable: true });
Object.defineProperty(WritableStreamDefaultController.prototype, Symbol.toStringTag, { value: 'WritableStreamDefaultController', configurable: true });

// ------------------------------------------------------------------------------------------------ pipeTo, pipeThrough

function RS_state(stream) { return host.rsState(stream); }

function readableStreamPipeTo(source, destPub, preventClose, preventAbort, preventCancel, signal) {
  const dest = wsOf(destPub, 'pipeTo');
  const reader = source.getReader();
  const writer = WS_acquireInternalWriter(dest);
  host.disturb(source);
  let shuttingDown = false;
  let currentWrite = resolved(undefined);
  return new Promise_((resolve, reject) => {
    let abortAlgorithm;
    if (signal !== undefined) {
      abortAlgorithm = () => {
        const error = signal.reason !== undefined ? signal.reason : new DOMExceptionC('Aborted', 'AbortError');
        const actions = [];
        if (!preventAbort) {
          actions.push(() => dest.state === 'writable' ? WS_abort(dest, error) : resolved(undefined));
        }
        if (!preventCancel) {
          actions.push(() => RS_state(source) === 'readable' ? reader.cancel(error) : resolved(undefined));
        }
        shutdownWithAction(() => Promise_.all(actions.map((a) => a())), true, error);
      };
      if (signal.aborted) { abortAlgorithm(); return; }
      signal.addEventListener('abort', abortAlgorithm);
    }

    function pipeLoop() {
      return new Promise_((resolveLoop, rejectLoop) => {
        function next(done) {
          if (done) resolveLoop(); else pipeStep().then(next, rejectLoop);
        }
        next(false);
      });
    }
    function pipeStep() {
      if (shuttingDown) return resolved(true);
      return writer.readyDeferred.promise.then(() => new Promise_((resolveRead, rejectRead) => {
        reader.read().then((result) => {
          if (result.done) { resolveRead(true); return; }
          currentWrite = WSW_write(writer, result.value);
          currentWrite.then(undefined, noop);
          resolveRead(false);
        }, rejectRead);
      }));
    }

    isOrBecomesErrored(source, reader.closed, (storedError) => {
      if (!preventAbort) shutdownWithAction(() => WS_abort(dest, storedError), true, storedError);
      else shutdown(true, storedError);
    });
    isOrBecomesErroredW(dest, writer.closedDeferred.promise, (storedError) => {
      if (!preventCancel) shutdownWithAction(() => reader.cancel(storedError), true, storedError);
      else shutdown(true, storedError);
    });
    isOrBecomesClosed(source, reader.closed, () => {
      if (!preventClose) shutdownWithAction(() => WSW_closeWithErrorPropagation(writer));
      else shutdown();
    });
    if (WS_closeQueuedOrInFlight(dest) || dest.state === 'closed') {
      const destClosed = new TypeError('the destination writable stream closed before all data could be piped to it');
      if (!preventCancel) shutdownWithAction(() => reader.cancel(destClosed), true, destClosed);
      else shutdown(true, destClosed);
    }
    markHandled(pipeLoop());

    function waitForWritesToFinish() {
      const oldCurrentWrite = currentWrite;
      return currentWrite.then(() => oldCurrentWrite !== currentWrite ? waitForWritesToFinish() : undefined);
    }
    function isOrBecomesErrored(stream, promise, action) {
      if (RS_state(stream) === 'errored') action(host.rsError(stream)); else uponR(promise, action);
    }
    function isOrBecomesErroredW(stream, promise, action) {
      if (stream.state === 'errored') action(stream.storedError); else uponR(promise, action);
    }
    function isOrBecomesClosed(stream, promise, action) {
      if (RS_state(stream) === 'closed') action(); else uponF(promise, action);
    }
    function shutdownWithAction(action, originalIsError, originalError) {
      if (shuttingDown) return;
      shuttingDown = true;
      if (dest.state === 'writable' && !WS_closeQueuedOrInFlight(dest)) uponF(waitForWritesToFinish(), doTheRest);
      else doTheRest();
      function doTheRest() {
        let p;
        try { p = action(); } catch (e) { p = rejected(e); }
        upon(resolved(p), () => finalize(originalIsError, originalError), (newError) => finalize(true, newError));
      }
    }
    function shutdown(isError, error) {
      if (shuttingDown) return;
      shuttingDown = true;
      if (dest.state === 'writable' && !WS_closeQueuedOrInFlight(dest)) uponF(waitForWritesToFinish(), () => finalize(isError, error));
      else finalize(isError, error);
    }
    function finalize(isError, error) {
      WSW_release(writer);
      reader.releaseLock();
      if (signal !== undefined) signal.removeEventListener('abort', abortAlgorithm);
      if (isError) reject(error); else resolve(undefined);
    }
  });
}

function readPipeOptions(options) {
  if (options === undefined || options === null) options = {};
  else if (!isObject(options)) throw new TypeError('Failed to convert the options to a dictionary');
  const preventAbort = Boolean(options.preventAbort);
  const preventCancel = Boolean(options.preventCancel);
  const preventClose = Boolean(options.preventClose);
  const signal = options.signal;
  if (signal !== undefined && !(signal instanceof AbortSignalC)) throw new TypeError('options.signal is not an AbortSignal');
  return { preventAbort, preventCancel, preventClose, signal };
}

function pipeTo(source, destination, options) {
  let o;
  try {
    if (!(isObject(destination) && wsSlots.has(destination))) throw new TypeError('The destination is not a WritableStream');
    o = readPipeOptions(options);
  } catch (e) { return rejected(e); }
  if (source.locked) return rejected(new TypeError('Cannot pipe a locked stream'));
  if (wsSlots.get(destination).writer !== undefined) return rejected(new TypeError('Cannot pipe to a locked stream'));
  return readableStreamPipeTo(source, destination, o.preventClose, o.preventAbort, o.preventCancel, o.signal);
}

function pipeThrough(source, transform, options) {
  if (source.locked) throw new TypeError('Cannot pipe a locked stream');
  if (!isObject(transform)) throw new TypeError('The transform is not an object');
  const readable = transform.readable;
  const writable = transform.writable;
  if (!(isObject(readable) && host.isReadableStream(readable))) throw new TypeError('transform.readable is not a ReadableStream');
  if (!(isObject(writable) && wsSlots.has(writable))) throw new TypeError('transform.writable is not a WritableStream');
  const o = readPipeOptions(options);
  if (readable.locked) throw new TypeError('transform.readable is locked');
  if (wsSlots.get(writable).writer !== undefined) throw new TypeError('transform.writable is locked');
  markHandled(readableStreamPipeTo(source, writable, o.preventClose, o.preventAbort, o.preventCancel, o.signal));
  return readable;
}

// ------------------------------------------------------------------------------------------------ TransformStream

const tsSlots = new WeakMap();
const tctlSlots = new WeakMap();

function TS_initialize(ts, startDeferred, writableHWM, writableSizeAlgorithm, readableHWM, readableSizeAlgorithm) {
  const startAlgorithm = () => startDeferred.promise;
  const writeAlgorithm = (chunk) => TS_defaultSinkWrite(ts, chunk);
  const abortAlgorithm = (reason) => TS_defaultSinkAbort(ts, reason);
  const closeAlgorithm = () => TS_defaultSinkClose(ts);
  const ws = WS_new();
  const wsPub = ObjectCreate(WritableStream.prototype);
  ws.pub = wsPub;
  wsSlots.set(wsPub, ws);
  const wc = WSC_makeInternal();
  wc.pub = ObjectCreate(WritableStreamDefaultController.prototype);
  ctlSlots.set(wc.pub, wc);
  WSC_setUp(ws, wc, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, writableHWM, writableSizeAlgorithm);
  ts.writable = wsPub;
  ts.writableInternal = ws;

  const startR = () => startDeferred.promise;
  const pullAlgorithm = () => TS_defaultSourcePull(ts);
  const cancelAlgorithm = (reason) => TS_defaultSourceCancel(ts, reason);
  let rc;
  const readable = new RS({
    start(c) { rc = c; return startR(); },
    pull: pullAlgorithm,
    cancel: cancelAlgorithm,
  }, { highWaterMark: readableHWM, size: readableSizeAlgorithm === null ? undefined : readableSizeAlgorithm });
  ts.readable = readable;
  ts.readableController = rc;
  ts.backpressure = undefined;
  ts.backpressureChangeDeferred = undefined;
  TS_setBackpressure(ts, true);
  ts.controller = undefined;
}
function TS_error(ts, e) {
  ts.readableController.error(e);
  TS_errorWritableAndUnblockWrite(ts, e);
}
function TS_errorWritableAndUnblockWrite(ts, e) {
  TSC_clearAlgorithms(ts.controller);
  WSC_errorIfNeeded(ts.writableInternal.controller, e);
  TS_unblockWrite(ts);
}
function TS_unblockWrite(ts) {
  if (ts.backpressure) TS_setBackpressure(ts, false);
}
function TS_setBackpressure(ts, backpressure) {
  if (ts.backpressureChangeDeferred !== undefined) ts.backpressureChangeDeferred.resolve();
  ts.backpressureChangeDeferred = newDeferred();
  ts.backpressure = backpressure;
}
function TSC_setUp(ts, controller, transformAlgorithm, flushAlgorithm, cancelAlgorithm) {
  controller.stream = ts;
  ts.controller = controller;
  controller.transformAlgorithm = transformAlgorithm;
  controller.flushAlgorithm = flushAlgorithm;
  controller.cancelAlgorithm = cancelAlgorithm;
}
function TSC_setUpFromTransformer(ts, transformer, dict) {
  const controller = { pub: ObjectCreate(TransformStreamDefaultController.prototype), stream: undefined };
  tctlSlots.set(controller.pub, controller);
  let transformAlgorithm = (chunk) => {
    try { TSC_enqueue(controller, chunk); return resolved(undefined); } catch (e) { return rejected(e); }
  };
  let flushAlgorithm = () => resolved(undefined);
  let cancelAlgorithm = () => resolved(undefined);
  if (dict.transform !== undefined) transformAlgorithm = (chunk) => promiseCall(dict.transform, transformer, [chunk, controller.pub]);
  if (dict.flush !== undefined) flushAlgorithm = () => promiseCall(dict.flush, transformer, [controller.pub]);
  if (dict.cancel !== undefined) cancelAlgorithm = (reason) => promiseCall(dict.cancel, transformer, [reason]);
  TSC_setUp(ts, controller, transformAlgorithm, flushAlgorithm, cancelAlgorithm);
}
function TSC_clearAlgorithms(c) {
  c.transformAlgorithm = undefined; c.flushAlgorithm = undefined; c.cancelAlgorithm = undefined;
}
function TSC_enqueue(c, chunk) {
  const ts = c.stream;
  const rc = ts.readableController;
  if (!host.rsCanEnqueue(ts.readable)) throw new TypeError('Readable side is not in a state that permits enqueue');
  try { rc.enqueue(chunk); } catch (e) {
    TS_errorWritableAndUnblockWrite(ts, e);
    throw host.rsError(ts.readable);
  }
  const backpressure = !host.rsShouldPull(ts.readable);
  if (backpressure !== ts.backpressure) TS_setBackpressure(ts, true);
}
function TSC_error(c, e) { TS_error(c.stream, e); }
function TSC_performTransform(c, chunk) {
  const transformPromise_ = c.transformAlgorithm(chunk);
  return transformPromise(transformPromise_, undefined, (r) => { TS_error(c.stream, r); throw r; });
}
function TSC_terminate(c) {
  const ts = c.stream;
  if (host.rsCanEnqueue(ts.readable)) ts.readableController.close();
  const error = new TypeError('TransformStream terminated');
  TS_errorWritableAndUnblockWrite(ts, error);
}
function TS_defaultSinkWrite(ts, chunk) {
  if (ts.backpressure) {
    const bp = ts.backpressureChangeDeferred.promise;
    return bp.then(() => {
      const writable = ts.writableInternal;
      const state = writable.state;
      if (state === 'erroring') throw writable.storedError;
      return TSC_performTransform(ts.controller, chunk);
    });
  }
  return TSC_performTransform(ts.controller, chunk);
}
function TS_defaultSinkAbort(ts, reason) {
  const controller = ts.controller;
  if (controller.finishDeferred !== undefined) return controller.finishDeferred.promise;
  const readable = ts.readable;
  controller.finishDeferred = newDeferred();
  const cancelPromise = controller.cancelAlgorithm(reason);
  TSC_clearAlgorithms(controller);
  upon(cancelPromise, () => {
    if (RS_state(readable) === 'errored') controller.finishDeferred.reject(host.rsError(readable));
    else {
      ts.readableController.error(reason);
      controller.finishDeferred.resolve();
    }
  }, (r) => {
    ts.readableController.error(r);
    controller.finishDeferred.reject(r);
  });
  return controller.finishDeferred.promise;
}
function TS_defaultSinkClose(ts) {
  const controller = ts.controller;
  if (controller.finishDeferred !== undefined) return controller.finishDeferred.promise;
  const readable = ts.readable;
  controller.finishDeferred = newDeferred();
  const flushPromise = controller.flushAlgorithm();
  TSC_clearAlgorithms(controller);
  upon(flushPromise, () => {
    if (RS_state(readable) === 'errored') controller.finishDeferred.reject(host.rsError(readable));
    else {
      if (host.rsCanEnqueue(readable)) ts.readableController.close();
      controller.finishDeferred.resolve();
    }
  }, (r) => {
    ts.readableController.error(r);
    controller.finishDeferred.reject(r);
  });
  return controller.finishDeferred.promise;
}
function TS_defaultSourcePull(ts) {
  TS_setBackpressure(ts, false);
  return ts.backpressureChangeDeferred.promise;
}
function TS_defaultSourceCancel(ts, reason) {
  const controller = ts.controller;
  if (controller.finishDeferred !== undefined) return controller.finishDeferred.promise;
  const writable = ts.writableInternal;
  controller.finishDeferred = newDeferred();
  const cancelPromise = controller.cancelAlgorithm(reason);
  TSC_clearAlgorithms(controller);
  upon(cancelPromise, () => {
    if (writable.state === 'errored') controller.finishDeferred.reject(writable.storedError);
    else {
      WSC_errorIfNeeded(writable.controller, reason);
      TS_unblockWrite(ts);
      controller.finishDeferred.resolve();
    }
  }, (r) => {
    WSC_errorIfNeeded(writable.controller, r);
    TS_unblockWrite(ts);
    controller.finishDeferred.reject(r);
  });
  return controller.finishDeferred.promise;
}

function tsOf(o) {
  const s = isObject(o) ? tsSlots.get(o) : undefined;
  if (s === undefined) throw new TypeError('Illegal invocation: not a TransformStream');
  return s;
}
function tctlOf(o) {
  const s = isObject(o) ? tctlSlots.get(o) : undefined;
  if (s === undefined) throw new TypeError('Illegal invocation: not a TransformStreamDefaultController');
  return s;
}

class TransformStream {
  constructor(transformer = undefined, writableStrategy = undefined, readableStrategy = undefined) {
    if (transformer === undefined) transformer = null;
    writableStrategy = convertStrategy(writableStrategy);
    readableStrategy = convertStrategy(readableStrategy);
    let dict = {};
    if (transformer !== null) {
      if (!isObject(transformer)) throw new TypeError('The transformer is not an object');
      dict.cancel = transformer.cancel; validateFn(dict.cancel, 'cancel');
      dict.flush = transformer.flush; validateFn(dict.flush, 'flush');
      dict.readableType = transformer.readableType;
      dict.start = transformer.start; validateFn(dict.start, 'start');
      dict.transform = transformer.transform; validateFn(dict.transform, 'transform');
      dict.writableType = transformer.writableType;
    }
    if (dict.readableType !== undefined) throw new RangeError('Invalid readableType specified');
    if (dict.writableType !== undefined) throw new RangeError('Invalid writableType specified');
    const readableHWM = extractHighWaterMark(readableStrategy, 0);
    const readableSizeAlgorithm = readableStrategy.size === undefined ? null : extractSizeAlgorithm(readableStrategy);
    const writableHWM = extractHighWaterMark(writableStrategy, 1);
    const writableSizeAlgorithm = extractSizeAlgorithm(writableStrategy);
    const startDeferred = newDeferred();
    const ts = { pub: this };
    tsSlots.set(this, ts);
    TS_initialize(ts, startDeferred, writableHWM, writableSizeAlgorithm, readableHWM, readableSizeAlgorithm);
    TSC_setUpFromTransformer(ts, transformer === null ? {} : transformer, dict);
    if (dict.start !== undefined) startDeferred.resolve(dict.start.call(transformer, ts.controller.pub));
    else startDeferred.resolve(undefined);
  }
  get readable() { return tsOf(this).readable; }
  get writable() { return tsOf(this).writable; }
}

class TransformStreamDefaultController {
  constructor() { throw new TypeError('Illegal constructor'); }
  get desiredSize() {
    const c = tctlOf(this);
    const rc = c.stream.readableController;
    return rc.desiredSize;
  }
  enqueue(chunk = undefined) { TSC_enqueue(tctlOf(this), chunk); }
  error(reason = undefined) { TSC_error(tctlOf(this), reason); }
  terminate() { TSC_terminate(tctlOf(this)); }
}

for (const [C, names] of [
  [TransformStream, ['readable', 'writable']],
  [TransformStreamDefaultController, ['desiredSize', 'enqueue', 'error', 'terminate']],
]) {
  for (const n of names) {
    const d = Object.getOwnPropertyDescriptor(C.prototype, n);
    d.enumerable = true;
    Object.defineProperty(C.prototype, n, d);
  }
}
Object.defineProperty(TransformStream.prototype, Symbol.toStringTag, { value: 'TransformStream', configurable: true });
Object.defineProperty(TransformStreamDefaultController.prototype, Symbol.toStringTag, { value: 'TransformStreamDefaultController', configurable: true });

// ------------------------------------------------------------------------------------------------ TextEncoderStream, TextDecoderStream

const tesSlots = new WeakMap();
const tdsSlots = new WeakMap();

function makeGenericTransform(transformFn, flushFn) {
  // A TransformStream made from algorithms (the Streams Standard's "set up a TransformStream" for the encoding streams).
  const ts = new TransformStream({ transform: transformFn, flush: flushFn });
  return ts;
}

class TextEncoderStream {
  constructor() {
    let leading = null; // a pending high surrogate code unit
    const enc = new TE();
    const ts = makeGenericTransform(
      (chunk, controller) => {
        let input = `${chunk}`;
        if (leading !== null) { input = String.fromCharCode(leading) + input; leading = null; }
        let n = input.length;
        if (n > 0) {
          const last = input.charCodeAt(n - 1);
          if (last >= 0xD800 && last <= 0xDBFF) { leading = last; input = input.slice(0, n - 1); n--; }
        }
        if (n > 0) {
          const out = enc.encode(input); // lone surrogates become U+FFFD, a pair stays a pair
          if (out.length > 0) controller.enqueue(out);
        }
      },
      (controller) => {
        if (leading !== null) { leading = null; controller.enqueue(new Uint8Array([0xEF, 0xBF, 0xBD])); }
      });
    tesSlots.set(this, ts);
  }
  get encoding() { tesOf(this); return 'utf-8'; }
  get readable() { return tesOf(this).readable; }
  get writable() { return tesOf(this).writable; }
}
function tesOf(o) {
  const s = isObject(o) ? tesSlots.get(o) : undefined;
  if (s === undefined) throw new TypeError('Illegal invocation: not a TextEncoderStream');
  return s;
}

class TextDecoderStream {
  constructor(label = 'utf-8', options = undefined) {
    const decoder = new TD(label, options);
    const ts = makeGenericTransform(
      (chunk, controller) => {
        if (!(chunk instanceof ArrayBuffer_ || isView(chunk))) throw new TypeError('The chunk is not a BufferSource');
        const s = decoder.decode(chunk, { stream: true });
        if (s.length > 0) controller.enqueue(s);
      },
      (controller) => {
        const s = decoder.decode();
        if (s.length > 0) controller.enqueue(s);
      });
    tdsSlots.set(this, { ts, decoder });
  }
  get encoding() { return tdsOf(this).decoder.encoding; }
  get fatal() { return tdsOf(this).decoder.fatal; }
  get ignoreBOM() { return tdsOf(this).decoder.ignoreBOM; }
  get readable() { return tdsOf(this).ts.readable; }
  get writable() { return tdsOf(this).ts.writable; }
}
function tdsOf(o) {
  const s = isObject(o) ? tdsSlots.get(o) : undefined;
  if (s === undefined) throw new TypeError('Illegal invocation: not a TextDecoderStream');
  return s;
}

for (const [C, names] of [
  [TextEncoderStream, ['encoding', 'readable', 'writable']],
  [TextDecoderStream, ['encoding', 'fatal', 'ignoreBOM', 'readable', 'writable']],
]) {
  for (const n of names) {
    const d = Object.getOwnPropertyDescriptor(C.prototype, n);
    d.enumerable = true;
    Object.defineProperty(C.prototype, n, d);
  }
}
Object.defineProperty(TextEncoderStream.prototype, Symbol.toStringTag, { value: 'TextEncoderStream', configurable: true });
Object.defineProperty(TextDecoderStream.prototype, Symbol.toStringTag, { value: 'TextDecoderStream', configurable: true });

// ------------------------------------------------------------------------------------------------ transferable ReadableStream

// A ReadableStream travels as a MessagePort: the sender pumps chunks into the port, the receiver rebuilds a stream from the
// messages. Chunks are structured-cloned by the port. (The standard also passes pull/backpressure messages; here the sender
// reads as fast as the port accepts, so a transferred stream does not apply backpressure.)
// prepareTransfer makes the channel and returns the far port (the one that is transferred); transferReadable starts the pump
// once the serialization has succeeded; cancelTransfer drops the channel when it did not.
const pendingTransfers = new WeakMap();
function prepareTransfer(stream) {
  const { port1, port2 } = new MessageChannelC();
  pendingTransfers.set(stream, port1);
  return port2;
}
function cancelTransfer(stream) {
  const port1 = pendingTransfers.get(stream);
  pendingTransfers.delete(stream);
  if (port1 !== undefined) port1.close();
}
function transferReadable(stream) {
  const port1 = pendingTransfers.get(stream);
  pendingTransfers.delete(stream);
  const reader = stream.getReader();
  function pump() {
    reader.read().then((r) => {
      if (r.done) { port1.postMessage({ type: 'close' }); port1.close(); return; }
      try { port1.postMessage({ type: 'chunk', value: r.value }); } catch (e) {
        port1.postMessage({ type: 'error', reason: e }); reader.cancel(e).then(undefined, noop); port1.close(); return;
      }
      pump();
    }, (e) => { port1.postMessage({ type: 'error', reason: e }); port1.close(); });
  }
  port1.onmessage = (ev) => {
    if (ev.data.type === 'cancel') { reader.cancel(ev.data.reason).then(undefined, noop); port1.close(); }
  };
  pump();
}
function receiveReadable(port) {
  let controller;
  return new RS({
    start(c) {
      controller = c;
      port.onmessage = (ev) => {
        const d = ev.data;
        if (d.type === 'chunk') controller.enqueue(d.value);
        else if (d.type === 'close') { controller.close(); port.close(); }
        else if (d.type === 'error') { controller.error(d.reason); port.close(); }
      };
    },
    cancel(reason) { try { port.postMessage({ type: 'cancel', reason }); } catch (e) {} port.close(); },
  }, { highWaterMark: 0 });
}

return {
  WritableStream, WritableStreamDefaultWriter, WritableStreamDefaultController,
  TransformStream, TransformStreamDefaultController, TextEncoderStream, TextDecoderStream,
  pipeTo, pipeThrough, prepareTransfer, cancelTransfer, transferReadable, receiveReadable,
};
})
