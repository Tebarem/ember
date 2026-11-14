// preact_diff: keyed children and component state on a mounted 1 000-row table. Each step changes
// every 10th row's label, swaps two rows, and re-renders the same tree, so the reconciler reuses
// the existing row components (each is a class component with its own state). Needs dom_stub.js
// and Preact 10 before this file.
//
// Deviations from the shared workload description:
//  - Class state, not hooks. The vendored UMD bundle (tests/js/frameworks/preact-10.29.8.umd.js)
//    has no hooks, so each row is a class component. Its state is set once and never changed.
//  - One update and swap per step() call, not 100 per run. The driver runs a few dozen steps per
//    process run (0.5 s warm-up, then 15 timed batches of about 20 ms each). A step that did 100
//    updates would take about 2.3 s, which is past the 50 ms to 2 s budget per run.
var h;
var render;
var root;
var rows;
var version;

function Row(props) {
  return h(
    'tr',
    null,
    h('td', null, String(props.id)),
    h('td', null, props.label)
  );
}

class StatefulRow extends preact.Component {
  constructor(props) {
    super(props);
    this.state = { hits: 0 };
  }
  render(props) {
    return Row(props);
  }
}

function Table(props) {
  var list = [];
  for (var i = 0; i < props.rows.length; i++) {
    var r = props.rows[i];
    list.push(h(StatefulRow, { key: r.id, id: r.id, label: r.label }));
  }
  return h('table', null, h('tbody', null, list));
}

function setup() {
  h = preact.h;
  render = preact.render;
  root = document.createElement('div');
  rows = [];
  for (var i = 0; i < 1000; i++) {
    rows.push({ id: i, label: 'row ' + i });
  }
  version = 0;
  render(h(Table, { rows: rows }), root);
}

function step() {
  version++;
  for (var i = 0; i < rows.length; i += 10) {
    rows[i].label = 'row ' + rows[i].id + ' v' + version;
  }
  var a = (version * 7) % rows.length;
  var b = (version * 13 + 5) % rows.length;
  var t = rows[a];
  rows[a] = rows[b];
  rows[b] = t;
  render(h(Table, { rows: rows }), root);
  var tbody = root.firstChild.firstChild;
  var labelLen = tbody.childNodes[a].childNodes[1].firstChild.data.length;
  return version * 7919 + labelLen * 31 + rows[0].id;
}
