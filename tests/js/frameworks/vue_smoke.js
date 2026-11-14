// Smoke script for Vue reactivity 3.5 (vue-reactivity-3.5.45.global.js, loaded first). The final
// expression is 'ok' when every check passes, otherwise a short name of the failed check. The
// first statement queues a microtask on the engine's job queue; the test drains the queue and
// checks that the job ran (globalThis.smokeJob is 'done').
Promise.resolve().then(function () {
  globalThis.smokeJob = 'done';
});

(function () {
  var R = VueReactivity;

  // effect and reactive objects.
  var state = R.reactive({ count: 0 });
  var runs = 0;
  var seen = -1;
  R.effect(function () {
    runs++;
    seen = state.count;
  });
  state.count = 5;
  if (seen !== 5 || runs !== 2) {
    return 'effect';
  }

  // computed values follow their dependencies.
  var doubled = R.computed(function () {
    return state.count * 2;
  });
  state.count = 7;
  if (doubled.value !== 14) {
    return 'computed';
  }

  // Collections are tracked through their methods.
  var m = R.reactive(new Map());
  var size = -1;
  R.effect(function () {
    size = m.size;
  });
  m.set('a', 1);
  m.set('b', 2);
  if (size !== 2) {
    return 'map';
  }

  // Array mutations trigger length effects.
  var list = R.reactive([]);
  var len = -1;
  R.effect(function () {
    len = list.length;
  });
  list.push(1, 2, 3);
  if (len !== 3) {
    return 'array';
  }

  // Refs and readonly proxies.
  var r = R.ref(1);
  var got = 0;
  R.effect(function () {
    got = r.value;
  });
  r.value = 9;
  if (got !== 9) {
    return 'ref';
  }
  if (!R.isReadonly(R.readonly({ x: 1 }))) {
    return 'readonly';
  }
  return 'ok';
})();
