// array_methods: push, pop, map, filter, reduce, forEach, indexOf, slice, splice, sort and spread
// on arrays of 1e5 integers and objects, plus one holey array. Each step does a fixed amount of
// each operation, so the work per step is stable.
var ints;
var objs;
var holey;

function setup() {
  ints = [];
  for (var i = 0; i < 100000; i++) {
    ints.push(i);
  }
  objs = [];
  for (var j = 0; j < 100000; j++) {
    objs.push({ id: j, v: (j * 7919) % 100003 });
  }
  holey = new Array(10000);
  for (var k = 0; k < 10000; k += 2) {
    holey[k] = k;
  }
}

function step() {
  var sum = 0;

  for (var p = 0; p < 1000; p++) {
    ints.push(p);
  }
  for (var q = 0; q < 1000; q++) {
    sum = (sum + ints.pop()) | 0;
  }

  var mapped = ints.map(function (x) {
    return x * 2;
  });
  sum = (sum + mapped.filter(function (x) {
    return x % 3 === 0;
  }).reduce(function (a, b) {
    return (a + b) | 0;
  }, 0)) | 0;

  var counted = 0;
  ints.forEach(function (x) {
    counted += x & 1;
  });
  sum = (sum + counted + ints.indexOf(99999)) | 0;

  var sliced = ints.slice(0, 50000);
  sum = (sum + sliced.length) | 0;

  var copy = ints.slice(0, 20000);
  copy.splice(1000, 10, -1, -2, -3);
  sum = (sum + copy.length + copy[1000]) | 0;

  var small = objs.slice(0, 3000);
  small.sort(function (a, b) {
    return a.v - b.v;
  });
  sum = (sum + small[0].id + small[2999].v) | 0;

  var spread = [...ints.slice(0, 1000), ...holey];
  sum = (sum + spread.length) | 0;

  var holeyHits = 0;
  holey.forEach(function () {
    holeyHits++;
  });
  var holeyMapped = holey.map(function (x) {
    return x + 1;
  });
  sum = (sum + holeyHits + holeyMapped.length) | 0;

  return sum;
}
