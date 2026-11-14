// preact_render: mount a 1 000-row table component tree into the stub DOM, then unmount it. Each
// step is one full mount (the reconciler creates every node). Needs dom_stub.js and Preact 10
// before this file (compare.sh adds them).
var h;
var render;
var root;

function Row(props) {
  return h(
    'tr',
    { class: props.sel ? 'danger' : '' },
    h('td', { class: 'id' }, String(props.id)),
    h('td', null, 'row ' + props.id),
    h('td', null, h('a', { href: '#' + props.id }, 'link'))
  );
}

function Table(props) {
  var rows = [];
  for (var i = 0; i < props.n; i++) {
    rows.push(h(Row, { key: i, id: i, sel: i % 10 === 0 }));
  }
  return h('table', null, h('tbody', null, rows));
}

function setup() {
  h = preact.h;
  render = preact.render;
  root = document.createElement('div');
}

function step() {
  render(h(Table, { n: 1000 }), root);
  var tbody = root.firstChild.firstChild;
  var count = tbody.childNodes.length;
  render(null, root);
  return count;
}
