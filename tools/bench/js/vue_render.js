// vue_render: a render function over @vue/reactivity that builds plain vnode objects for a list
// of 1 000 rows and diffs them against the previous vnodes, patching a plain "DOM". A computed
// renders the list when it is read. Each step changes 50 rows through reactive writes, then reads
// the render and diffs it. Needs vue-reactivity-3.5.45.global.js before this file.
var state;
var renderVnodes;
var prev;
var dom;
var changes;
var round;

function renderList(items) {
  var out = [];
  for (var i = 0; i < items.length; i++) {
    var it = items[i];
    out.push({
      type: 'tr',
      key: it.id,
      children: [
        { type: 'td', text: String(it.id) },
        { type: 'td', text: it.label + (it.done ? ' (done)' : '') }
      ]
    });
  }
  return out;
}

function diff(oldList, newList, target) {
  var n = 0;
  for (var i = 0; i < newList.length; i++) {
    var next = newList[i];
    var old = oldList[i];
    if (old && old.key === next.key && old.children[1].text === next.children[1].text) {
      continue;
    }
    target[i] = next;
    n++;
  }
  return n;
}

function setup() {
  var R = VueReactivity;
  var items = [];
  for (var i = 0; i < 1000; i++) {
    items.push({ id: i, label: 'item ' + i, done: i % 3 === 0 });
  }
  state = R.reactive({ items: items });
  renderVnodes = R.computed(function () {
    return renderList(state.items);
  });
  prev = renderVnodes.value;
  dom = prev.slice();
  changes = 0;
  round = 0;
}

function step() {
  round++;
  for (var k = 0; k < 50; k++) {
    var idx = (round * 37 + k * 19) % 1000;
    state.items[idx].label = 'item ' + idx + ' r' + round;
    state.items[idx].done = (round + k) % 2 === 0;
  }
  var next = renderVnodes.value;
  changes += diff(prev, next, dom);
  prev = next;
  return (changes * 31 + dom[round % 1000].children[1].text.length) % 2147483647;
}
