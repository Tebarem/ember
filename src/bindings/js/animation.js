// The JS-written part of the animations (W6c): requestAnimationFrame, the Web Animations API (Animation, KeyframeEffect,
// AnimationEffect, AnimationTimeline, DocumentTimeline, AnimationPlaybackEvent, Element.animate, getAnimations), the CSS
// animation and transition events (AnimationEvent, TransitionEvent) and the CSSAnimation and CSSTransition classes.
// Embedded with #embed by impl_anim.c and evaluated once per realm, the first time any of it is used; the completion value
// is a function that takes the host object (the natives of impl_anim.c) and returns the exports.
//
// The timing, the play state machine and the sampling are in C (src/anim): an Animation here is the promises, the events and
// the argument conversion around the record with the id of its handle. The C side sends the notifications to notify().
//
// Known gaps (W6e): animations and transitions of pseudo-elements (::before, ::after) are not supported. The pseudoElement
// option of Element.animate / KeyframeEffect is stored and read back but the effect applies to nothing, the CSS events
// report pseudoElement '', and the pseudo-element style computation does not go through the animator hook. The classes are
// hand-written here instead of generated from an IDL, so brand checks and attribute enumerability differ from WebIDL.
(function (host) {
'use strict';
const EventC = host.Event;
const EventTargetC = host.EventTarget;
const DOMExceptionC = host.DOMException;
const Promise_ = Promise;
const WeakRef_ = WeakRef;
const isFinite_ = Number.isFinite;

// Field and operation numbers of the natives (impl_anim.c).
const F_CURRENT_TIME = 0, F_START_TIME = 1, F_PLAY_STATE = 2, F_PENDING = 3, F_REPLACE_STATE = 4, F_PLAYBACK_RATE = 5,
  F_RELEVANT = 6, F_READY_RESOLVED = 7, F_FINISHED_RESOLVED = 8, F_HAS_EFFECT = 9, F_PENDING_RATE = 10, F_KIND = 11,
  F_HAS_TIMELINE = 12;
const O_PLAY = 0, O_PAUSE = 1, O_REVERSE = 2, O_FINISH = 3, O_CANCEL = 4, O_SET_CURRENT_TIME = 5, O_SET_START_TIME = 6,
  O_SET_PLAYBACK_RATE = 7, O_UPDATE_PLAYBACK_RATE = 8, O_PERSIST = 9, O_SET_TIMELINE = 10;
// Notifications (EMBI_ANN_*).
const N_READY_NEW = 0, N_READY = 1, N_FINISHED_NEW = 2, N_FINISHED = 3, N_CANCELED = 4, N_EV_FINISH = 5, N_EV_CANCEL = 6,
  N_EV_REMOVE = 7, N_RELEASE = 8, N_RETAIN = 9, N_DESTROYED = 10;
const K_WEB = 0, K_CSSA = 1, K_CSST = 2;

const PLAY_STATES = ['idle', 'running', 'paused', 'finished'];
const REPLACE_STATES = ['active', 'removed', 'persisted'];
const FILLS = ['none', 'forwards', 'backwards', 'both', 'auto'];
const DIRECTIONS = ['normal', 'reverse', 'alternate', 'alternate-reverse'];
const COMPOSITES = ['replace', 'add', 'accumulate'];
const CEV = ['animationstart', 'animationiteration', 'animationend', 'animationcancel', 'transitionrun', 'transitionstart',
  'transitionend', 'transitioncancel'];

const ADOPT = Symbol('adopt');

function throwDOM(name, msg) { throw new DOMExceptionC(msg || name, name); }
function isObject(v) { return (typeof v === 'object' && v !== null) || typeof v === 'function'; }
function deferred() {
  const d = { done: false };
  d.promise = new Promise_((resolve, reject) => {
    d.resolve = (v) => { d.done = true; resolve(v); };
    d.reject = (e) => { d.done = true; reject(e); };
  });
  return d;
}
function resolvedDeferred(v) { const d = deferred(); d.resolve(v); return d; }
function toDouble(v, what) {
  const n = Number(v);
  if (!isFinite_(n)) throw new TypeError('Failed to set the ' + what + ': The provided double value is non-finite.');
  return n;
}

// ------------------------------------------------------------------ requestAnimationFrame

let rafId = 0;
const rafs = new Map();
let snap = new Map();
function raf(cb) {
  const id = ++rafId;
  rafs.set(id, cb);
  host.rafCount(rafs.size);
  return id;
}
function caf(id) {
  id = Number(id);
  if (rafs.delete(id)) host.rafCount(rafs.size);
  snap.delete(id);
}
function beginFrame() {
  snap = new Map(rafs);
  rafs.clear();
  host.rafCount(0);
}
// Runs the next callback of the frame's snapshot; true while more are left.
function runNext(ts) {
  const it = snap.entries().next();
  if (it.done) return false;
  const [id, cb] = it.value;
  snap.delete(id);
  try { cb(ts); } catch (e) { host.report(e); }
  return snap.size > 0;
}

// ------------------------------------------------------------------ the registry of Animation objects

const strong = new Map(); // id -> Animation while relevant
const weak = new Map();   // id -> WeakRef of every wrapper alive
let sinceSweep = 0;
function lookup(id) {
  let a = strong.get(id);
  if (a !== undefined) return a;
  const w = weak.get(id);
  if (w === undefined) return undefined;
  a = w.deref();
  if (a === undefined) weak.delete(id);
  return a;
}
function register(id, a) {
  weak.set(id, new WeakRef_(a));
  if (++sinceSweep >= 512) {
    sinceSweep = 0;
    for (const [k, w] of weak) if (w.deref() === undefined) weak.delete(k);
  }
}

// ------------------------------------------------------------------ timing

function parseTimingInput(opts, kfEffect) {
  const t = { delay: 0, endDelay: 0, fill: 'auto', iterationStart: 0, iterations: 1, duration: 'auto', direction: 'normal',
    easing: 'linear' };
  if (opts === undefined || opts === null) return t;
  if (typeof opts !== 'object' && typeof opts !== 'function') {
    // a number is the duration
    const d = Number(opts);
    if (typeof opts === 'string' && opts === 'auto') return t;
    if (d !== d || d < 0) throw new TypeError('Invalid duration');
    t.duration = d;
    return t;
  }
  applyTiming(t, opts);
  return t;
}
function applyTiming(t, o) {
  if (o.delay !== undefined) t.delay = toDouble(o.delay, 'delay');
  if (o.endDelay !== undefined) t.endDelay = toDouble(o.endDelay, 'endDelay');
  if (o.fill !== undefined) {
    const f = String(o.fill);
    if (!FILLS.includes(f)) throw new TypeError("Failed to set the 'fill' property: '" + f + "' is not a valid value.");
    t.fill = f;
  }
  if (o.iterationStart !== undefined) {
    const v = Number(o.iterationStart);
    if (!isFinite_(v) || v < 0) throw new TypeError("Failed to set the 'iterationStart' property: Invalid value.");
    t.iterationStart = v;
  }
  if (o.iterations !== undefined) {
    const v = Number(o.iterations);
    if (v !== v || v < 0) throw new TypeError("Failed to set the 'iterations' property: Invalid value.");
    t.iterations = v;
  }
  if (o.duration !== undefined) {
    if (typeof o.duration === 'string') {
      if (o.duration !== 'auto') throw new TypeError("Failed to set the 'duration' property: Invalid value.");
      t.duration = 'auto';
    } else {
      const v = Number(o.duration);
      if (v !== v || v < 0) throw new TypeError("Failed to set the 'duration' property: Invalid value.");
      t.duration = v;
    }
  }
  if (o.direction !== undefined) {
    const d = String(o.direction);
    if (!DIRECTIONS.includes(d)) throw new TypeError("Failed to set the 'direction' property: '" + d + "' is not a valid value.");
    t.direction = d;
  }
  if (o.easing !== undefined) {
    const e = String(o.easing);
    if (!host.easingOk(e)) throw new TypeError("Failed to set the 'easing' property: '" + e + "' is not a valid value.");
    t.easing = canonEasing(e);
  }
}
function timingArray(t) {
  const fill = t.fill === 'forwards' ? 2 : t.fill === 'backwards' ? 1 : t.fill === 'both' ? 3 : 0;
  return [t.delay, t.endDelay, fill, t.iterationStart, t.iterations, t.duration === 'auto' ? 0 : t.duration,
    DIRECTIONS.indexOf(t.direction), t.easing === 'linear' ? '' : t.easing];
}

// ------------------------------------------------------------------ keyframes

function cssName(k) {
  if (k.startsWith('--')) return k;
  if (k === 'cssFloat') return 'float';
  if (k === 'cssOffset') return 'offset';
  return k.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase());
}
function jsName(css) {
  if (css.startsWith('--')) return css;
  if (css === 'float') return 'cssFloat';
  return css.replace(/-([a-z])/g, (m, c) => c.toUpperCase());
}
// The serialization of an easing the Web Animations way: keywords lowercase (CSS escapes resolved), step-start and
// step-end as steps(), the jump term end omitted.
function canonEasing(e) {
  const s = e.replace(/\\([0-9a-fA-F]{1,6})[ \t\n]?/g, (m, h) => String.fromCodePoint(parseInt(h, 16))).trim();
  const l = s.toLowerCase();
  if (/^[a-z-]+$/.test(l)) {
    if (l === 'step-start') return 'steps(1, start)';
    if (l === 'step-end') return 'steps(1)';
    return l;
  }
  const m = /^steps\(\s*(\d+)\s*(?:,\s*([a-z-]+)\s*)?\)$/.exec(l);
  if (m) return m[2] === undefined || m[2] === 'end' ? 'steps(' + m[1] + ')' : 'steps(' + m[1] + ', ' + m[2] + ')';
  return s;
}
function checkEasing(e) {
  e = String(e);
  if (!host.easingOk(e)) throw new TypeError("Invalid easing '" + e + "'");
  return canonEasing(e);
}
const PSEUDOS = new Set(['before', 'after', 'marker', 'backdrop', 'first-line', 'first-letter', 'placeholder', 'selection',
  'file-selector-button', 'cue', 'highlight', 'part', 'slotted', 'view-transition', 'view-transition-group',
  'view-transition-image-pair', 'view-transition-old', 'view-transition-new', 'grammar-error', 'spelling-error',
  'target-text', 'details-content', 'scroll-marker', 'scroll-marker-group', 'checkmark', 'picker-icon', 'picker']);
// The pseudoElement attribute: null, or a pseudo-element selector (the legacy single-colon forms of ::before, ::after,
// ::first-line and ::first-letter included), lowercased; anything else is a SyntaxError.
function canonPseudo(v) {
  if (v === null) return null;
  const s = String(v);
  const m = /^(::?)([A-Za-z-]+)(\(.*\))?$/.exec(s);
  if (m) {
    const n = m[2].toLowerCase();
    const legacy = m[1] === ':' && (n === 'before' || n === 'after' || n === 'first-line' || n === 'first-letter');
    if ((m[1] === '::' || legacy) && PSEUDOS.has(n)) return m[1] + n + (m[3] || '');
  }
  throwDOM('SyntaxError', "'" + s + "' is not a valid pseudo-element selector.");
}
function checkOffset(o) {
  if (o === null || o === undefined) return null;
  const n = Number(o);
  if (!isFinite_(n) || n < 0 || n > 1) throw new TypeError('Keyframe offsets must be between 0 and 1.');
  return n;
}
function checkComposite(c) {
  if (c === undefined) return 'auto';
  c = String(c);
  if (c !== 'auto' && !COMPOSITES.includes(c)) throw new TypeError("Invalid composite '" + c + "'");
  return c;
}
// The keys the keyframes argument reads are IDL attribute names (camelCase, cssFloat, cssOffset) or custom properties.
function idlName(k) {
  if (k.startsWith('--')) return true;
  return k.indexOf('-') < 0 && k !== 'float' && k !== 'offset';
}
const SKIP = new Set(['offset', 'easing', 'composite', 'computedOffset']);

// Converts the keyframes argument into [{offset|null, easing, composite, props:[[css, text]]}].
function processKeyframes(arg) {
  if (arg === null || arg === undefined) return [];
  if (!isObject(arg)) throw new TypeError('Keyframes must be an object or an array of objects.');
  const out = [];
  if (typeof arg[Symbol.iterator] === 'function') {
    // (index loops: a throw inside a for-of over an iterator trips an assertion of the engine)
    const items = Array.from(arg);
    for (let ii = 0; ii < items.length; ii++) {
      const item = items[ii];
      if (!isObject(item)) throw new TypeError('Keyframes must be objects.');
      const kf = { offset: checkOffset(item.offset), easing: item.easing === undefined ? 'linear' : checkEasing(item.easing),
        composite: checkComposite(item.composite), props: [] };
      const ks = Object.keys(item);
      for (let ki = 0; ki < ks.length; ki++) {
        const k = ks[ki];
        if (SKIP.has(k) || !idlName(k) || !host.animatable(cssName(k))) continue;
        const cn = cssName(k), text = String(item[k]);
        if (host.valueOk(cn, text)) kf.props.push([cn, text]);
      }
      out.push(kf);
    }
  } else {
    const props = [];
    let n = 0;
    const ks = Object.keys(arg);
    for (let ki = 0; ki < ks.length; ki++) {
      const k = ks[ki];
      if (SKIP.has(k) || !idlName(k) || !host.animatable(cssName(k))) continue;
      let v = arg[k];
      if (typeof v === 'string' || typeof v === 'number' || !(v && typeof v[Symbol.iterator] === 'function')) v = [v];
      else v = Array.from(v);
      const cn = cssName(k);
      props.push([cn, v.map(String)]);
      if (v.length > n) n = v.length;
    }
    let offsets = arg.offset;
    if (offsets === undefined || offsets === null) offsets = [];
    else if (typeof offsets === 'number' || typeof offsets === 'string') offsets = [offsets];
    else offsets = Array.from(offsets);
    let eases = arg.easing;
    if (eases === undefined) eases = ['linear'];
    else if (typeof eases === 'string') eases = [eases];
    else eases = Array.from(eases);
    for (let ei = 0; ei < eases.length; ei++) checkEasing(eases[ei]);
    let comps = arg.composite;
    if (comps === undefined) comps = ['auto'];
    else if (typeof comps === 'string') comps = [comps];
    else comps = Array.from(comps);
    for (let j = 0; j < n; j++) {
      out.push({ offset: j < offsets.length ? checkOffset(offsets[j]) : null,
        easing: checkEasing(eases.length ? eases[j % eases.length] : 'linear'),
        composite: checkComposite(comps.length ? comps[j % comps.length] : 'auto'), props: [] });
    }
    for (let pi = 0; pi < props.length; pi++) {
      const name = props[pi][0], vals = props[pi][1];
      for (let j = 0; j < vals.length; j++) if (host.valueOk(name, vals[j])) out[j].props.push([name, vals[j]]);
    }
  }
  let last = 0;
  for (let i = 0; i < out.length; i++) {
    const kf = out[i];
    if (kf.offset !== null) {
      if (kf.offset < last) throw new TypeError('Keyframes with specified offsets must be in ascending order.');
      last = kf.offset;
    }
  }
  return out;
}
// Computed offsets (the spacing of the Web Animations spec).
function computedOffsets(kfs) {
  const n = kfs.length;
  const res = kfs.map((k) => k.offset);
  if (n === 0) return res;
  if (res[n - 1] === null) res[n - 1] = 1;
  if (res[0] === null) res[0] = 0;
  let i = 0;
  while (i < n) {
    if (res[i] !== null) { i++; continue; }
    const a = i - 1;
    let b = i;
    while (res[b] === null) b++;
    for (let j = a + 1; j < b; j++) res[j] = res[a] + (res[b] - res[a]) * (j - a) / (b - a);
    i = b;
  }
  return res;
}
function flatKeyframes(kfs) {
  const flat = [];
  for (let i = 0; i < kfs.length; i++) {
    const k = kfs[i];
    for (const [p, v] of k.props) flat.push(i, k.offset === null ? NaN : k.offset, k.easing === 'linear' ? '' : k.easing, p, v, k.composite === 'replace' ? 1 : k.composite === 'add' ? 2 : k.composite === 'accumulate' ? 3 : 0);
  }
  return flat;
}

// ------------------------------------------------------------------ the timeline

class AnimationTimeline {
  constructor() { throw new TypeError('Illegal constructor'); }
  get currentTime() { return host.time(); }
  get duration() { return null; }
}
const timelineKey = Symbol('timeline');
class DocumentTimeline extends AnimationTimeline {
  constructor(options) {
    const o = options === undefined ? 0 : (options && options.originTime !== undefined ? Number(options.originTime) : 0);
    return DocumentTimeline._make(o); // the base class constructor is not called: it throws
  }
  static _make(origin) {
    const t = Object.create(DocumentTimeline.prototype);
    timelineOrigins.set(t, origin);
    return t;
  }
  get currentTime() { return host.time() - (timelineOrigins.get(this) || 0); }
}
const timelineOrigins = new WeakMap();
const docTimeline = DocumentTimeline._make(0);
function documentTimeline() { return docTimeline; }

// ------------------------------------------------------------------ the effects

class AnimationEffect {
  constructor() { throw new TypeError('Illegal constructor'); }
  getTiming() { return effTiming(this); }
  getComputedTiming() { return effComputed(this); }
  updateTiming(timing) { effUpdate(this, timing); }
}
const eff = new WeakMap(); // effect -> internal record
function rec(e) {
  const r = eff.get(e);
  if (r === undefined) throw new TypeError('Illegal invocation');
  return r;
}
function effTiming(e) {
  const t = rec(e).timing;
  return { delay: t.delay, endDelay: t.endDelay, fill: t.fill, iterationStart: t.iterationStart, iterations: t.iterations,
    duration: t.duration, direction: t.direction, easing: t.easing };
}
// The C side counts in seconds: a sum like 0.05 + 0.1 + 0.01 shows its rounding error in ms.
function round6(x) { return Math.round(x * 1e6) / 1e6; }
function effComputed(e) {
  const r = rec(e);
  const t = r.timing;
  const dur = t.duration === 'auto' ? 0 : t.duration;
  let active = dur * t.iterations;
  if (active !== active) active = 0;
  const out = { startTime: 0, delay: t.delay, endDelay: t.endDelay, fill: t.fill === 'auto' ? 'none' : t.fill, iterationStart: t.iterationStart,
    iterations: t.iterations, duration: dur, direction: t.direction, easing: t.easing,
    endTime: Math.max(t.delay + active + t.endDelay, 0), activeDuration: active, localTime: null, progress: null,
    currentIteration: null };
  if (r.anim !== null) {
    const c = host.computed(r.anim.id);
    if (c !== undefined) {
      out.endTime = round6(c[0]); out.activeDuration = round6(c[1]); out.localTime = c[2] === null ? null : round6(c[2]);
      out.progress = c[3]; out.currentIteration = c[4];
    }
  }
  return out;
}
function effUpdate(e, timing) {
  const r = rec(e);
  const t = Object.assign({}, r.timing);
  if (timing !== undefined && timing !== null) applyTiming(t, timing);
  r.timing = t;
  if (r.anim !== null) host.setTiming(r.anim.id, timingArray(t));
}
function effPush(r) {
  const a = r.anim;
  if (a === null) return;
  const comp = r.composite === 'auto' ? 0 : COMPOSITES.indexOf(r.composite);
  const icomp = r.iterationComposite === 'accumulate' ? 2 : 0;
  host.setEffect(a.id, r.target, flatKeyframes(r.kfs), timingArray(r.timing), comp, icomp);
}

class KeyframeEffect extends AnimationEffect {
  constructor(target, keyframes, options) {
    // AnimationEffect's constructor throws, so the instance is made here without calling it.
    const self = Object.create(new.target.prototype);
    if (target instanceof KeyframeEffect) {
      const s = rec(target);
      eff.set(self, { target: s.target, pseudo: s.pseudo, kfs: s.kfs.map((k) => ({ offset: k.offset, easing: k.easing,
        composite: k.composite, props: k.props.slice() })), timing: Object.assign({}, s.timing), composite: s.composite,
        iterationComposite: s.iterationComposite, anim: null });
      return self;
    }
    if (target !== null && !(target instanceof Element)) {
      throw new TypeError("Failed to construct 'KeyframeEffect': parameter 1 is not of type 'Element'.");
    }
    const timing = parseTimingInput(options, true);
    let composite = 'replace', iterationComposite = 'replace', pseudo = null;
    if (isObject(options)) {
      if (options.composite !== undefined) {
        composite = String(options.composite);
        if (composite !== 'auto' && !COMPOSITES.includes(composite)) throw new TypeError('Invalid composite');
      }
      if (options.iterationComposite !== undefined) iterationComposite = String(options.iterationComposite);
      if (options.pseudoElement !== undefined) pseudo = canonPseudo(options.pseudoElement);
    }
    eff.set(self, { target, pseudo, kfs: processKeyframes(keyframes), timing, composite, iterationComposite, anim: null });
    return self;
  }
  get target() { return rec(this).target; }
  set target(v) {
    const r = rec(this);
    if (v !== null && !(v instanceof Element)) throw new TypeError("Failed to set the 'target' property: not an Element.");
    r.target = v;
    if (r.anim !== null) host.setTarget(r.anim.id, v);
  }
  get pseudoElement() { return rec(this).pseudo; }
  set pseudoElement(v) { rec(this).pseudo = canonPseudo(v); }
  get composite() { return rec(this).composite; }
  set composite(v) {
    v = String(v);
    if (v !== 'auto' && !COMPOSITES.includes(v)) return;
    const r = rec(this);
    r.composite = v;
    effPush(r);
  }
  get iterationComposite() { return rec(this).iterationComposite; }
  set iterationComposite(v) { const r = rec(this); r.iterationComposite = String(v); effPush(r); }
  getKeyframes() {
    const r = rec(this);
    const offs = computedOffsets(r.kfs);
    return r.kfs.map((k, i) => {
      const o = { offset: k.offset, computedOffset: offs[i], easing: k.easing, composite: k.composite };
      for (const [p, v] of k.props) o[jsName(p)] = v;
      return o;
    });
  }
  setKeyframes(keyframes) {
    const r = rec(this);
    r.kfs = processKeyframes(keyframes);
    effPush(r);
  }
}
// The classes keep their documented names and lengths.
Object.defineProperty(KeyframeEffect, 'length', { value: 2 });

// ------------------------------------------------------------------ Animation

const anims = new WeakMap(); // Animation -> internal state
function st(a) {
  const s = anims.get(a);
  if (s === undefined) throw new TypeError('Illegal invocation');
  return s;
}
function playbackEvent(type, init) {
  return new AnimationPlaybackEvent(type, init);
}

class Animation extends EventTargetC {
  constructor(effect, timeline, token, handle, id) {
    super();
    let s;
    if (effect === ADOPT) {
      s = { h: handle, id, effect: null, timeline: docTimeline, ready: resolvedDeferred(this), finished: deferred(), idStr: '',
        handlers: {} };
      anims.set(this, s);
      if (host.get(id, F_FINISHED_RESOLVED)) s.finished.resolve(this);
      return;
    }
    const h = host.create();
    const hid = host.id(h);
    s = { h, id: hid, effect: null, timeline: null, ready: resolvedDeferred(this), finished: deferred(), idStr: '', handlers: {} };
    anims.set(this, s);
    register(hid, this);
    if (timeline === undefined) timeline = docTimeline;
    if (timeline !== null && !(timeline instanceof AnimationTimeline)) {
      throw new TypeError("Failed to construct 'Animation': parameter 2 is not of type 'AnimationTimeline'.");
    }
    s.timeline = timeline;
    if (timeline === null) host.op(hid, O_SET_TIMELINE, 0);
    if (effect !== undefined && effect !== null) setEffect(this, s, effect);
  }
  get id() { return st(this).idStr; }
  set id(v) { st(this).idStr = String(v); }
  get effect() { return st(this).effect; }
  set effect(e) {
    const s = st(this);
    if (e !== null && !(e instanceof AnimationEffect)) throw new TypeError("Failed to set the 'effect' property: not an AnimationEffect.");
    if (s.effect === e) return;
    setEffect(this, s, e);
  }
  get timeline() { return st(this).timeline; }
  set timeline(t) {
    const s = st(this);
    if (t !== null && !(t instanceof AnimationTimeline)) throw new TypeError("Failed to set the 'timeline' property.");
    s.timeline = t;
    host.op(s.id, O_SET_TIMELINE, t === null ? 0 : 1);
  }
  get startTime() { return host.get(st(this).id, F_START_TIME); }
  set startTime(v) {
    const s = st(this);
    const n = v === null ? NaN : toDouble(v, 'startTime');
    thr(host.op(s.id, O_SET_START_TIME, n));
  }
  get currentTime() { return host.get(st(this).id, F_CURRENT_TIME); }
  set currentTime(v) {
    const s = st(this);
    if (v === null) {
      if (host.get(s.id, F_CURRENT_TIME) !== null) throw new TypeError("Failed to set the 'currentTime' property: currentTime may not be changed from resolved to unresolved.");
      return;
    }
    thr(host.op(s.id, O_SET_CURRENT_TIME, toDouble(v, 'currentTime')));
  }
  get playbackRate() { return host.get(st(this).id, F_PLAYBACK_RATE); }
  set playbackRate(v) { host.op(st(this).id, O_SET_PLAYBACK_RATE, toDouble(v, 'playbackRate')); }
  get playState() { return PLAY_STATES[host.get(st(this).id, F_PLAY_STATE)]; }
  get replaceState() { return REPLACE_STATES[host.get(st(this).id, F_REPLACE_STATE)]; }
  get pending() { return host.get(st(this).id, F_PENDING); }
  // Animation.overallProgress: the current time as a fraction of the end time of the effect, in 0..1; null without an effect
  // or a current time.
  get overallProgress() {
    const s = st(this);
    const ct = host.get(s.id, F_CURRENT_TIME);
    if (s.effect === null || ct === null) return null;
    const end = s.effect.getComputedTiming().endTime;
    if (end === 0) return ct < 0 ? 0 : 1;
    if (end === Infinity) return 0;
    return Math.min(Math.max(ct / end, 0), 1);
  }
  get ready() { return st(this).ready.promise; }
  get finished() { return st(this).finished.promise; }
  get onfinish() { return st(this).handlers.finish || null; }
  set onfinish(f) { setHandler(this, 'finish', f); }
  get oncancel() { return st(this).handlers.cancel || null; }
  set oncancel(f) { setHandler(this, 'cancel', f); }
  get onremove() { return st(this).handlers.remove || null; }
  set onremove(f) { setHandler(this, 'remove', f); }
  play() { thr(host.op(st(this).id, O_PLAY)); }
  pause() { thr(host.op(st(this).id, O_PAUSE)); }
  reverse() { thr(host.op(st(this).id, O_REVERSE)); }
  finish() { thr(host.op(st(this).id, O_FINISH)); }
  cancel() { host.op(st(this).id, O_CANCEL); }
  persist() { host.op(st(this).id, O_PERSIST); }
  updatePlaybackRate(rate) { thr(host.op(st(this).id, O_UPDATE_PLAYBACK_RATE, toDouble(rate, 'playbackRate'))); }
  commitStyles() {
    const s = st(this);
    const e = s.effect;
    if (e === null || e.target === null) return;
    if (!e.target.isConnected) throwDOM('InvalidStateError', 'Target element is not rendered.');
    const cs = getComputedStyle(e.target);
    const ps = rec(e).kfs.flatMap((k) => k.props);
    for (let i = 0; i < ps.length; i++) e.target.style.setProperty(ps[i][0], cs.getPropertyValue(ps[i][0]));
  }
}
function thr(err) {
  if (err === undefined) return;
  if (err === 'TypeError') throw new TypeError('Invalid operation on the animation.');
  throwDOM(err, 'The animation is in an invalid state for this operation.');
}
function setHandler(a, type, f) {
  const s = st(a);
  if (s.wrappers === undefined) s.wrappers = {};
  if (s.wrappers[type]) a.removeEventListener(type, s.wrappers[type]);
  if (typeof f === 'function') {
    const w = (ev) => f.call(a, ev);
    s.wrappers[type] = w;
    s.handlers[type] = f;
    a.addEventListener(type, w);
  } else {
    s.wrappers[type] = undefined;
    s.handlers[type] = null;
  }
}
function setEffect(a, s, e) {
  const old = s.effect;
  if (old !== null) {
    const r = rec(old);
    r.anim = null;
    if (e === null) host.clearEffect(s.id);
  }
  if (e === null) { s.effect = null; return; }
  const r = rec(e);
  if (r.anim !== null && r.anim !== s) {
    const other = lookup(r.anim.id);
    if (other !== undefined) other.effect = null;
  }
  r.anim = s;
  s.effect = e;
  effPush(r);
}

class CSSAnimation extends Animation {
  #name = '';
  constructor(token, handle, id, name) { super(ADOPT, undefined, undefined, handle, id); this.#name = name; }
  get animationName() { return this.#name; }
}
class CSSTransition extends Animation {
  #prop = '';
  constructor(token, handle, id, name) { super(ADOPT, undefined, undefined, handle, id); this.#prop = name; }
  get transitionProperty() { return this.#prop; }
}

// An Animation for a record that has none (a CSS animation or transition, or a Web animation that outlived its object).
function wrap(id) {
  let a = lookup(id);
  if (a !== undefined) return a;
  const info = host.cssInfo(id);
  if (info === undefined) return null;
  const h = host.adopt(id);
  if (h === null) return null;
  if (info[0] === K_CSSA) a = new CSSAnimation(ADOPT, h, id, info[1]);
  else if (info[0] === K_CSST) a = new CSSTransition(ADOPT, h, id, info[1]);
  else a = new Animation(ADOPT, undefined, undefined, h, id);
  const s = anims.get(a);
  s.h = h;
  register(id, a);
  if (host.get(id, F_RELEVANT)) strong.set(id, a);
  if (info[0] !== K_WEB) {
    // The effect of a CSS animation: a read-only view of the record.
    const e = Object.create(KeyframeEffect.prototype);
    eff.set(e, { target: host.target(id), pseudo: null, kfs: [], timing: { delay: 0, endDelay: 0, fill: 'both', iterationStart: 0,
      iterations: 1, duration: 'auto', direction: 'normal', easing: 'linear' }, composite: 'replace', iterationComposite: 'replace',
      anim: s });
    s.effect = e;
  }
  return a;
}
function idsToAnimations(ids) {
  const out = [];
  for (const id of ids) {
    const a = wrap(id);
    if (a) out.push(a);
  }
  return out;
}

// ------------------------------------------------------------------ the notifications of the animator

function notify(id, what, time) {
  const a = lookup(id);
  if (a === undefined) return;
  const s = anims.get(a);
  switch (what) {
    case N_READY_NEW: s.ready = deferred(); break;
    case N_READY: if (!s.ready.done) s.ready.resolve(a); break;
    case N_FINISHED_NEW: s.finished = deferred(); break;
    case N_FINISHED: if (!s.finished.done) s.finished.resolve(a); break;
    case N_CANCELED: {
      if (!s.ready.done) {
        s.ready.promise.catch(() => {});
        s.ready.reject(new DOMExceptionC('The animation was aborted.', 'AbortError'));
      }
      s.ready = resolvedDeferred(a);
      if (!s.finished.done) {
        s.finished.promise.catch(() => {});
        s.finished.reject(new DOMExceptionC('The animation was aborted.', 'AbortError'));
      }
      s.finished = deferred();
      break;
    }
    case N_EV_FINISH:
      a.dispatchEvent(playbackEvent('finish', { currentTime: host.get(id, F_CURRENT_TIME), timelineTime: host.time() }));
      break;
    case N_EV_CANCEL:
      a.dispatchEvent(playbackEvent('cancel', { currentTime: null, timelineTime: host.time() }));
      break;
    case N_EV_REMOVE:
      a.dispatchEvent(playbackEvent('remove', { currentTime: null, timelineTime: host.time() }));
      break;
    case N_RETAIN: strong.set(id, a); break;
    case N_RELEASE: strong.delete(id); break;
    case N_DESTROYED: strong.delete(id); weak.delete(id); break;
  }
}

// ------------------------------------------------------------------ events

class AnimationPlaybackEvent extends EventC {
  #ct = null;
  #tt = null;
  constructor(type, init) {
    super(type, init);
    if (init) {
      this.#ct = init.currentTime === undefined ? null : init.currentTime;
      this.#tt = init.timelineTime === undefined ? null : init.timelineTime;
    }
  }
  get currentTime() { return this.#ct; }
  get timelineTime() { return this.#tt; }
}
// elapsedTime is an IDL double: a value that does not convert to a finite number is a TypeError.
function finiteDouble(v, what) {
  const n = Number(v);
  if (!isFinite_(n)) throw new TypeError("Failed to construct '" + what + "': The provided double value is non-finite.");
  return n;
}
class AnimationEvent extends EventC {
  #name = '';
  #elapsed = 0;
  #pseudo = '';
  #animation = null;
  constructor(type, init) {
    if (arguments.length === 0) throw new TypeError("Failed to construct 'AnimationEvent': 1 argument required, but only 0 present.");
    super(type, init);
    if (init) {
      if (init.animationName !== undefined) this.#name = String(init.animationName);
      if (init.elapsedTime !== undefined) this.#elapsed = finiteDouble(init.elapsedTime, 'AnimationEvent');
      if (init.pseudoElement !== undefined) this.#pseudo = String(init.pseudoElement);
      if (init.animation !== undefined) this.#animation = init.animation;
    }
  }
  get animation() { return this.#animation; }
  get animationName() { return this.#name; }
  get elapsedTime() { return this.#elapsed; }
  get pseudoElement() { return this.#pseudo; }
}
class TransitionEvent extends EventC {
  #name = '';
  #elapsed = 0;
  #pseudo = '';
  constructor(type, init) {
    if (arguments.length === 0) throw new TypeError("Failed to construct 'TransitionEvent': 1 argument required, but only 0 present.");
    super(type, init);
    if (init) {
      if (init.propertyName !== undefined) this.#name = String(init.propertyName);
      if (init.elapsedTime !== undefined) this.#elapsed = finiteDouble(init.elapsedTime, 'TransitionEvent');
      if (init.pseudoElement !== undefined) this.#pseudo = String(init.pseudoElement);
    }
  }
  get propertyName() { return this.#name; }
  get elapsedTime() { return this.#elapsed; }
  get pseudoElement() { return this.#pseudo; }
}
function cssEvent(el, kind, name, elapsed) {
  const type = CEV[kind];
  const ev = kind < 4
    ? new AnimationEvent(type, { bubbles: true, animationName: name, elapsedTime: elapsed })
    : new TransitionEvent(type, { bubbles: true, propertyName: name, elapsedTime: elapsed });
  el.dispatchEvent(ev);
}

// ------------------------------------------------------------------ Element and Document

function elementAnimate(el, keyframes, options) {
  const effect = new KeyframeEffect(el, keyframes, options);
  const anim = new Animation(effect, docTimeline);
  if (isObject(options) && options.id !== undefined) anim.id = options.id;
  anim.play();
  return anim;
}
function elementGetAnimations(el, options) {
  const subtree = isObject(options) && !!options.subtree;
  return idsToAnimations(host.list(el, subtree));
}
function documentGetAnimations() {
  return idsToAnimations(host.list(null, true));
}

for (const C of [Animation, KeyframeEffect, AnimationEffect, AnimationTimeline, DocumentTimeline, AnimationPlaybackEvent,
  AnimationEvent, TransitionEvent, CSSAnimation, CSSTransition]) {
  Object.defineProperty(C.prototype, Symbol.toStringTag, { value: C.name, configurable: true });
}

return { raf, caf, beginFrame, runNext, notify, cssEvent, elementAnimate, elementGetAnimations, documentGetAnimations,
  documentTimeline, Animation, KeyframeEffect, AnimationEffect, AnimationTimeline, DocumentTimeline,
  AnimationPlaybackEvent, AnimationEvent, TransitionEvent, CSSAnimation, CSSTransition };
})
