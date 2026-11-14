// Smoke script for Preact 10 (preact-10.29.8.umd.js, loaded first, with dom_stub.js). The final
// expression is 'ok' when every check passes, otherwise a short name of the failed check. The
// first statement queues a microtask on the engine's job queue; the test drains the queue and
// checks that the job ran (globalThis.smokeJob is 'done').
Promise.resolve().then(function () {
  globalThis.smokeJob = 'done';
});

(function () {
  var h = preact.h;
  var render = preact.render;
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

  render(null, root);
  if (root.childNodes.length !== 0) {
    return 'unmount';
  }
  return 'ok';
})();
