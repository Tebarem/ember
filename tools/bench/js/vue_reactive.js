// vue_reactive: a reactivity graph of 1 000 nodes built with @vue/reactivity: 100 refs, then 900
// computeds, each reading two earlier nodes chosen by a fixed LCG (so the graph depth stays
// logarithmic), with 10 effects on the top nodes. Each step performs 2 000 writes to the refs and
// then reads the effect results. Needs vue-reactivity-3.5.45.global.js before this file.
var refs;
var nodes;
var effects;
var seen;
var writes;

function lcg(state) {
  return (Math.imul(state, 1103515245) + 12345) & 0x7fffffff;
}

function setup() {
  var R = VueReactivity;
  refs = [];
  nodes = [];
  var seed = 42;
  for (var i = 0; i < 100; i++) {
    var ref = R.ref(i);
    refs.push(ref);
    nodes.push(ref);
  }
  for (var j = 100; j < 1000; j++) {
    seed = lcg(seed);
    var a = nodes[seed % j];
    seed = lcg(seed);
    var b = nodes[seed % j];
    nodes.push(
      R.computed(
        (function (x, y) {
          return function () {
            return (x.value + y.value) % 1000003;
          };
        })(a, b)
      )
    );
  }
  seen = [];
  effects = [];
  for (var e = 0; e < 10; e++) {
    seen.push(0);
    effects.push(
      R.effect(
        (function (idx, node) {
          return function () {
            seen[idx] = node.value;
          };
        })(e, nodes[999 - e * 37])
      )
    );
  }
  writes = 0;
}

function step() {
  for (var k = 0; k < 2000; k++) {
    writes++;
    refs[k % 100].value = (writes * 3 + k) % 1000003;
  }
  var sum = 0;
  for (var e = 0; e < seen.length; e++) {
    sum = (sum + seen[e]) % 1000003;
  }
  return sum;
}
