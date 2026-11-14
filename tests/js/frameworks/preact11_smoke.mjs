// Smoke module for Preact 11 (ESM, preact-11/preact.mjs and preact-11/hooks/hooks.mjs). It is the
// entry module of tests/unit/js/test_frameworks_esm.c, which loads it through the embedder's
// module loader after dom_stub.js has run as a script. The module sets globalThis.preact11Result
// to 'ok' when every check passes, otherwise to a short name of the failed check.
import { h, render } from './preact-11/preact.mjs';
import { useEffect, useMemo, useState } from './preact-11/hooks/hooks.mjs';

function check() {
  var root = document.createElement('div');

  render(h('div', { id: 'app', class: 'a' }, h('span', null, 'hi'), 'text'), root);
  var div = root.firstChild;
  if (!div || div.localName !== 'div') {
    return 'element';
  }
  if (div.getAttribute('id') !== 'app' || div.getAttribute('class') !== 'a') {
    return 'attributes';
  }
  var span = div.firstChild;
  if (span.localName !== 'span' || span.firstChild.data !== 'hi') {
    return 'child';
  }
  if (div.childNodes[1].data !== 'text') {
    return 'text';
  }

  // A second render diffs against the existing tree: the same element is reused and updated.
  render(h('div', { id: 'b' }, 'x'), root);
  if (root.firstChild !== div) {
    return 'node reuse';
  }
  if (div.getAttribute('id') !== 'b' || div.childNodes.length !== 1 || div.firstChild.data !== 'x') {
    return 'diff';
  }

  // A function component.
  function Hello(props) {
    return h('p', null, 'Hello ' + props.name);
  }
  render(h(Hello, { name: 'Ember' }), root);
  if (root.firstChild.localName !== 'p' || root.firstChild.firstChild.data !== 'Hello Ember') {
    return 'component';
  }

  // A hook from hooks.mjs, which imports "preact" by its bare name: the memo factory runs once.
  var calls = 0;
  function Memo(props) {
    var v = useMemo(function () {
      calls++;
      return props.n * 2;
    }, [props.n]);
    return h('i', null, String(v));
  }
  render(h(Memo, { n: 21 }), root);
  if (root.firstChild.localName !== 'i' || root.firstChild.firstChild.data !== '42') {
    return 'hook';
  }
  if (calls !== 1) {
    return 'hook memo';
  }

  render(null, root);
  if (root.childNodes.length !== 0) {
    return 'unmount';
  }
  return 'ok';
}

// A component with state and an effect. Preact runs its effects after paint, which the stub
// document models as a timer (dom_stub.js queues it), so flushTimers runs them. A state update
// renders in a microtask (Preact's debounce uses queueMicrotask).
async function checkStateEffect() {
  var root = document.createElement('div');
  var setCount = null;
  var effectRuns = [];
  function Counter(props) {
    var st = useState(0);
    var count = st[0];
    setCount = st[1];
    useEffect(function () {
      effectRuns.push(count);
    }, [count]);
    return h('b', { class: 'count' }, 'count ' + count);
  }

  render(h(Counter, null), root);
  var b = root.firstChild;
  if (!b || b.localName !== 'b' || b.firstChild.data !== 'count 0') {
    return 'state initial';
  }
  flushTimers();
  if (effectRuns.length !== 1 || effectRuns[0] !== 0) {
    return 'effect first';
  }

  setCount(function (c) {
    return c + 1;
  });
  await null;
  if (root.firstChild !== b || b.firstChild.data !== 'count 1') {
    return 'state update';
  }
  flushTimers();
  if (effectRuns.length !== 2 || effectRuns[1] !== 1) {
    return 'effect update';
  }

  render(null, root);
  if (root.childNodes.length !== 0) {
    return 'state unmount';
  }
  return 'ok';
}

var result = check();
if (result === 'ok') {
  result = await checkStateEffect();
}
globalThis.preact11Result = result;
